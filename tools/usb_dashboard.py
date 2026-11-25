#!/usr/bin/env python3
"""Serve the SCRATCHSOR dashboard on loopback and carry its API/events over USB."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass, field
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import queue
import re
import sqlite3
import threading
import time
from urllib.parse import urlsplit
import webbrowser
from batch_sessions import SessionError, SessionRecorder

PREFIX = b"@USB "
SERIAL_BAUD = 921600
MAX_LINE = 4096
MAX_RESPONSE = 1024 * 1024
DATA = Path(__file__).resolve().parents[1] / "data"


class FrameReader:
    def __init__(self):
        self.buffer = bytearray()
        self.dropping = False

    def feed(self, data):
        packets = []
        for byte in data:
            if byte == 10:
                if not self.dropping and self.buffer.startswith(PREFIX):
                    try:
                        packet = json.loads(self.buffer[len(PREFIX):])
                        if isinstance(packet, dict):
                            packets.append(packet)
                    except (ValueError, UnicodeError):
                        pass
                self.buffer.clear()
                self.dropping = False
            elif not self.dropping:
                if len(self.buffer) >= MAX_LINE:
                    self.buffer.clear()
                    self.dropping = True
                else:
                    self.buffer.append(byte)
        return packets


def select_port(explicit, ports):
    if explicit:
        return explicit
    if len(ports) != 1:
        found = ", ".join(ports) or "none"
        raise ValueError(f"Specify --port COM7 (detected serial ports: {found}).")
    return ports[0]


def open_serial(port):
    import serial
    stream = serial.Serial(baudrate=SERIAL_BAUD, timeout=0.02, write_timeout=2)
    # Avoid intentionally toggling the board's reset/download-mode pins.
    stream.dtr = False
    stream.rts = False
    stream.port = port
    stream.open()
    return stream


@dataclass
class Response:
    id: int
    ready: threading.Event = field(default_factory=threading.Event)
    data: bytearray = field(default_factory=bytearray)
    part: int = 0
    status: int = 0
    type: str = ""
    error: Exception | None = None


class SerialBridge:
    def __init__(self, port, open_port=open_serial):
        self.port, self.open_port = port, open_port
        self.stream = None
        self.pending = None
        self.connected = False
        self.requests = threading.Lock()
        self.lock = threading.RLock()
        self.subscribers = set()
        self.stop = threading.Event()
        self.opened = threading.Event()
        self.next_id = 0
        self.thread = threading.Thread(target=self._read, daemon=True)

    def start(self):
        self.thread.start()
        self.opened.wait(2)

    def close(self):
        self.stop.set()
        with self.lock:
            if self.stream:
                self.stream.close()
            self._fail(ConnectionError("USB bridge stopped"))
        self.thread.join(3)

    def _fail(self, error):
        if self.pending and not self.pending.ready.is_set():
            self.pending.error = error
            self.pending.ready.set()

    def _write(self, packet):
        data = PREFIX + json.dumps(packet, ensure_ascii=True, separators=(",", ":")).encode() + b"\n"
        if len(data) - 1 > MAX_LINE:
            raise ValueError("Request exceeds the 4096-byte USB message limit")
        with self.lock:
            if not self.stream:
                raise ConnectionError("ESP32 USB serial port is disconnected")
            try:
                if self.stream.write(data) != len(data):
                    raise OSError("incomplete USB write")
            except OSError as e:
                raise ConnectionError("USB write failed; command outcome is unknown") from e

    def request(self, path, method="GET", body="", token="", timeout=60):
        if not path.startswith("/api/v1/") or "?" in path or method not in ("GET", "POST"):
            raise ValueError("Unsupported API request")
        # Check size before opening a pending request or writing any bytes.
        packet = {"id": 4294967295, "path": path, "method": method, "body": body, "token": token}
        if len(PREFIX) + len(json.dumps(packet, ensure_ascii=True, separators=(",", ":")).encode()) > MAX_LINE:
            raise ValueError("Request exceeds the 4096-byte USB message limit")
        if not self.requests.acquire(timeout=10):
            raise TimeoutError("USB is busy with another request")
        try:
            with self.lock:
                self.next_id = self.next_id % 4294967295 + 1
                response = Response(self.next_id)
                self.pending = response
            packet["id"] = response.id
            self._write(packet)
            if not response.ready.wait(timeout):
                raise TimeoutError("No complete USB response; check firmware/connection. Command outcome may be unknown.")
            if response.error:
                raise response.error
            if response.type.split(";", 1)[0] == "application/json":
                try:
                    json.loads(response.data)
                except ValueError as e:
                    raise ConnectionError("Device returned incomplete or invalid JSON") from e
            return response.status, response.type, bytes(response.data)
        finally:
            with self.lock:
                self.pending = None
            self.requests.release()

    def _dispatch(self, packet):
        with self.lock:
            event = packet.get("event")
            if isinstance(event, dict) and isinstance(event.get("t"), str):
                if not self.connected:
                    self.connected = True
                    self.publish({"t": "usb", "connected": True})
                self.publish(event)
                return
            r = self.pending
            if not r or packet.get("id") != r.id or r.ready.is_set():
                return
            status, content_type, body = packet.get("status"), packet.get("type"), packet.get("body")
            if (type(status) is not int or not 100 <= status <= 599 or
                    not isinstance(content_type, str) or not content_type or
                    any(ord(c) < 32 or ord(c) > 126 for c in content_type) or
                    not isinstance(body, str) or type(packet.get("done")) is not bool or
                    type(packet.get("part")) is not int or packet["part"] != r.part or
                    (r.part and (status != r.status or content_type != r.type))):
                self._fail(ConnectionError("Invalid or missing USB response chunk; download/command is incomplete"))
                return
            data = body.encode("utf-8")
            if len(r.data) + len(data) > MAX_RESPONSE:
                self._fail(ConnectionError("USB response exceeds 1 MiB limit"))
                return
            r.status, r.type = status, content_type
            r.data.extend(data)
            r.part += 1
            if packet["done"]:
                r.ready.set()

    def _read(self):
        while not self.stop.is_set():
            try:
                stream = self.open_port(self.port)
                with self.lock:
                    self.stream = stream
                self.opened.set()
                reader = FrameReader()
                last_subscribe = 0
                last_packet = time.monotonic()
                while not self.stop.is_set():
                    now = time.monotonic()
                    with self.lock:
                        listening = bool(self.subscribers)
                    if listening and now - last_subscribe >= 2:
                        self._write({"subscribe": True})
                        last_subscribe = now
                    for packet in reader.feed(stream.read(1024)):
                        self._dispatch(packet)
                        last_packet = time.monotonic()
                    if self.connected and now - last_packet > 5:
                        self.connected = False
                        self.publish({"t": "usb", "connected": False})
            except (OSError, ValueError) as e:
                with self.lock:
                    self._fail(ConnectionError(f"USB disconnected: {e}. A sent command may already have run."))
            finally:
                with self.lock:
                    if self.stream:
                        self.stream.close()
                    self.stream = None
                    self.connected = False
                self.publish({"t": "usb", "connected": False})
            self.stop.wait(1)

    @contextmanager
    def events(self, maxsize=32):
        events = queue.Queue(maxsize=maxsize)
        with self.lock:
            self.subscribers.add(events)
            events.put({"t": "usb", "connected": self.connected})
        try:
            yield events
        finally:
            with self.lock:
                self.subscribers.discard(events)

    def publish(self, event):
        with self.lock:
            for events in self.subscribers:
                if events.full():
                    try:
                        events.get_nowait()
                    except queue.Empty:
                        pass
                events.put_nowait(event)


class DashboardHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(DATA), **kwargs)

    def end_headers(self):
        path = getattr(self, 'path', '')
        if not path.startswith('/api/') and path != '/usb/events':
            self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def _allowed(self):
        port = self.server.server_address[1]
        hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}
        if self.headers.get("Host") not in hosts:
            self.send_error(403, "Use the loopback dashboard address")
            return False
        for name in ("Origin", "Referer"):
            value = self.headers.get(name)
            if value:
                try:
                    url = urlsplit(value)
                    allowed = url.scheme == "http" and url.netloc in hosts and not url.username
                except ValueError:
                    allowed = False
                if not allowed:
                    self.send_error(403, "Foreign browser origin")
                    return False
        return True

    def do_GET(self):
        if not self._allowed():
            return
        if self.path == "/usb/events":
            return self._events()
        if self.path.startswith("/api/"):
            return self._api("GET")
        # Keep the PC collection workflow as its own landing page. The ESP's
        # root opens the phone product; both assets remain available here.
        if self.path in ("/", "/index.html"):
            self.path = "/legacy.html"
        super().do_GET()

    def do_HEAD(self):
        if self._allowed():
            super().do_HEAD()

    def do_POST(self):
        if self._allowed():
            self._api("POST")

    def do_OPTIONS(self):
        if self._allowed():
            self.send_error(405)

    def list_directory(self, path):
        self.send_error(404)

    def _api(self, method):
        try:
            if self.headers.get("Transfer-Encoding"):
                raise ValueError("Chunked request bodies are unsupported")
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 <= length <= MAX_LINE:
                raise ValueError("Invalid or oversized request body")
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError("Incomplete request body")
            filename = None
            if self.path == '/api/v1/sessions' or self.path.startswith('/api/v1/sessions/'):
                status, kind, data, filename = self._sessions(method, body)
            else:
                if method == 'POST' and self.path in ('/api/v1/record', '/api/v1/restart', '/api/v1/factory_reset') and self.server.sessions.capture:
                    raise SessionError('Save the batch trial before changing the board recording or restarting.', 409)
                status, kind, data = self.server.bridge.request(
                    self.path, method, body.decode("utf-8"), self.headers.get("X-SCRATCHSOR-Token", ""))
        except SessionError as e:
            return self._json_error(e.status, str(e), e.code)
        except (ValueError, UnicodeError) as e:
            return self._json_error(400, str(e))
        except TimeoutError as e:
            return self._json_error(504, str(e))
        except ConnectionError as e:
            return self._json_error(503, str(e))
        except (sqlite3.Error, OSError) as e:
            return self._json_error(500, f'Cannot save or read the PC session: {e}')
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        if filename:
            self.send_header('Content-Disposition', f'attachment; filename="{filename}"')
        elif self.path in ("/api/v1/export.csv", "/api/v1/export.json", "/api/v1/calibration.csv") and status == 200:
            self.send_header("Content-Disposition", f'attachment; filename="{self.path.rsplit("/", 1)[1]}"')
        self.end_headers()
        self.wfile.write(data)

    def _sessions(self, method, raw):
        store = self.server.sessions
        parts = self.path[len('/api/v1/sessions'):].strip('/').split('/')
        parts = parts if parts != [''] else []
        body = json.loads(raw) if method == 'POST' else {}
        if not isinstance(body, dict):
            raise SessionError('JSON object required.')
        session = parts[0] if parts else None
        selected = None
        if not parts:
            if method == 'POST':
                session = store.create_session(body.get('name'))
        elif len(parts) == 1 and method == 'GET':
            pass
        elif len(parts) == 2 and parts[1] in ('rename', 'remove') and method == 'POST':
            if parts[1] == 'rename':
                store.rename_session(session, body.get('name'))
            else:
                store.remove_session(session)
                session = None
        elif len(parts) == 2 and parts[1] == 'distances' and method == 'POST':
            selected = store.add_distance(session, body.get('distanceCm'))
        elif len(parts) == 2 and parts[1] == 'record' and method == 'POST':
            if type(body.get('distanceId')) is not int:
                raise SessionError('Select a distance section before recording.')
            store.start_stream(session, body['distanceId'])
        elif len(parts) == 2 and parts[1] in ('retry', 'recover') and method == 'POST':
            (store.retry if parts[1] == 'retry' else store.reconcile)(session)
        elif len(parts) == 3 and parts[1] == 'trials' and parts[2].isdigit() and method == 'POST':
            store.include_trial(session, int(parts[2]), body.get('included'))
        elif len(parts) == 3 and parts[1] == 'trials' and parts[2].isdigit() and method == 'GET':
            return 200, 'application/json', json.dumps(store.trial_data(session, int(parts[2]))).encode(), None
        elif len(parts) == 4 and parts[1] in ('distances', 'trials') and parts[2].isdigit() and parts[3] in ('rename', 'remove') and method == 'POST':
            item = int(parts[2])
            if parts[1] == 'distances':
                if parts[3] == 'rename':
                    store.rename_distance(session, item, body.get('distanceCm'))
                else:
                    store.remove_distance(session, item)
            elif parts[3] == 'rename':
                store.rename_trial(session, item, body.get('name'))
            else:
                store.remove_trial(session, item)
        elif len(parts) == 2 and parts[1] == 'export.zip' and method == 'GET':
            name = store.snapshot(session)['session']['name']
            safe = re.sub(r'[^A-Za-z0-9_-]+', '_', name).strip('_')[:60] or 'magnet_session'
            return 200, 'application/zip', store.export_all(session), f'{safe}_{session[:8]}.zip'
        elif len(parts) == 3 and parts[1] == 'distances' and parts[2].endswith('.csv') and parts[2][:-4].isdigit() and method == 'GET':
            distance = int(parts[2][:-4])
            data = store.export_distance(session, distance)
            group = next(d for d in store.snapshot(session)['session']['distances'] if d['id'] == distance)
            return 200, 'text/csv; charset=utf-8', data, f'distance_{group["label"]}cm.csv'
        else:
            raise SessionError('Session endpoint not found.', 404)
        snapshot = store.snapshot(session)
        if selected is not None:
            snapshot['selectedDistanceId'] = selected
        return 200, 'application/json', json.dumps(snapshot).encode(), None

    def _json_error(self, status, detail, code='usb_error'):
        data = json.dumps({"error": code, "detail": detail}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _events(self):
        try:
            with self.server.bridge.events() as events:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                while not self.server.bridge.stop.is_set():
                    try:
                        event = events.get(timeout=2)
                        message = "data: " + json.dumps(event, ensure_ascii=True) + "\n\n"
                    except queue.Empty:
                        message = ": heartbeat\n\n"
                    self.wfile.write(message.encode())
                    self.wfile.flush()
        except (ConnectionError, TimeoutError):
            pass

    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def log_message(self, format, *args):
        pass


class LiveDashboardHandler(DashboardHandler):
    """Receive-only surface: no device commands or recording database access."""
    def do_GET(self):
        if not self._allowed():
            return
        if self.path == '/usb/events':
            return self._events()
        if self.path == '/usb/info':
            data = json.dumps({'port': self.server.bridge.port, 'baud': SERIAL_BAUD}).encode()
            kind = 'application/json'
        elif self.path in ('/', '/live.html'):
            data = (DATA / 'live.html').read_bytes()
            kind = 'text/html; charset=utf-8'
        else:
            return self.send_error(404)
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if self._allowed():
            self._json_error(405, 'Live-only mode receives sensor data; device changes and recording are disabled.', 'live_only')

    def do_HEAD(self):
        if self._allowed():
            self.send_error(405)


def make_server(bridge, port=8765, session_path=None, live_only=False):
    handler = LiveDashboardHandler if live_only else DashboardHandler
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    server.bridge = bridge
    if not live_only:
        server.sessions = SessionRecorder(bridge, session_path or DATA.parent / 'recordings' / 'sessions.sqlite3')
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="ESP32 serial port, e.g. COM7; auto-select only when exactly one exists")
    parser.add_argument("--http-port", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--live-only", action="store_true", help="Receive live readings only; never open the recording database or send device API commands")
    args = parser.parse_args()
    try:
        from serial.tools.list_ports import comports
    except ImportError:
        parser.exit(1, "Install pyserial first: python -m pip install pyserial\n")
    try:
        ports = list(comports())
        if not args.port:
            usb_ports = [p for p in ports if p.vid is not None]
            if args.live_only or len(usb_ports) == 1:
                ports = usb_ports
        port = select_port(args.port, [p.device for p in ports])
        if not 1 <= args.http_port <= 65535:
            raise ValueError("HTTP port must be between 1 and 65535")
        bridge = SerialBridge(port)
        server = make_server(bridge, args.http_port, live_only=args.live_only)
    except (ValueError, OSError, sqlite3.Error) as e:
        parser.exit(1, f"{e}\n")
    bridge.start()
    url = f"http://127.0.0.1:{args.http_port}/"
    print(f"SCRATCHSOR USB dashboard: {url}\nSerial: {port} at {SERIAL_BAUD} baud. Ctrl+C to stop.", flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        bridge.close()


if __name__ == "__main__":
    main()
