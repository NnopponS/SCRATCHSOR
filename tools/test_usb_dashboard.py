"""USB bridge contract tests; no ESP32 is needed."""
import http.client
import json
import queue
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from usb_dashboard import DashboardHandler, FrameReader, SerialBridge, make_server, select_port


class Wire:
    """Serial boundary with a controlled firmware peer."""
    def __init__(self):
        self.rx = queue.Queue()
        self.written = queue.Queue()
        self.closed = False

    def read(self, size):
        if self.closed:
            raise OSError("unplugged")
        try:
            return self.rx.get(timeout=0.02)
        except queue.Empty:
            return b""

    def write(self, data):
        self.written.put(data)
        return len(data)

    def close(self):
        self.closed = True

    def packet(self, **fields):
        self.rx.put(b"@USB " + json.dumps(fields).encode() + b"\n")


class ProtocolTests(unittest.TestCase):
    def test_logs_fragmentation_malformed_and_oversized_lines(self):
        f = FrameReader()
        self.assertEqual(f.feed(b"[I] boot\n@USB {\"event\":"), [])
        self.assertEqual(f.feed(b"{\"t\":\"status\"}}\n"), [{"event": {"t": "status"}}])
        self.assertEqual(f.feed(b"@USB bad\n@USB []\n"), [])
        self.assertEqual(f.feed(b"x" * 20000), [])
        self.assertEqual(f.feed(b'@USB {"id":3}\n'), [])
        self.assertEqual(f.feed(b'@USB {"id":4}\n'), [{"id": 4}])

    def test_port_selection_never_guesses_between_devices(self):
        self.assertEqual(select_port(None, ["COM7"]), "COM7")
        self.assertEqual(select_port("COM9", ["COM7", "COM9"]), "COM9")
        for ports in ([], ["COM7", "COM9"]):
            with self.assertRaises(ValueError):
                select_port(None, ports)


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.wire = Wire()
        self.bridge = SerialBridge("COM7", open_port=lambda port: self.wire)
        self.bridge.start()
        self.addCleanup(self.bridge.close)

    def exchange(self, path, method="GET", body="", token=""):
        self.result = queue.Queue()

        def call():
            try:
                self.result.put(self.bridge.request(path, method, body, token, timeout=1))
            except Exception as e:
                self.result.put(e)

        t = threading.Thread(target=call)
        t.start()
        self.addCleanup(t.join, 2)
        return json.loads(self.wire.written.get(timeout=1)[5:])

    def test_full_response_chunks_unicode_and_token_forwarding(self):
        req = self.exchange("/api/v1/config", "POST", '{"name":"ทดสอบ"}', "secret")
        self.assertEqual((req["method"], req["body"], req["token"]),
                         ("POST", '{"name":"ทดสอบ"}', "secret"))
        self.wire.packet(id=req["id"] + 1, part=0, status=200, type="text/plain", body="wrong", done=True)
        self.wire.packet(id=req["id"], part=0, status=200, type="text/csv", body="seq,name\n1,ทดสอบ", done=False)
        self.wire.packet(id=req["id"], part=1, status=200, type="text/csv", body="\n", done=True)
        self.assertEqual(self.result.get(timeout=1), (200, "text/csv", "seq,name\n1,ทดสอบ\n".encode()))

    def test_authentication_error_is_not_rewritten_as_success(self):
        req = self.exchange("/api/v1/record", "POST", '{"active":true}')
        self.wire.packet(id=req["id"], part=0, status=401, type="application/json",
                         body='{"error":"unauthorized"}', done=True)
        status, _, body = self.result.get(timeout=1)
        self.assertEqual(status, 401)
        self.assertEqual(json.loads(body)["error"], "unauthorized")

    def test_missing_export_chunk_is_an_error(self):
        req = self.exchange("/api/v1/export.csv")
        self.wire.packet(id=req["id"], part=0, status=200, type="text/csv", body="header\n", done=False)
        self.wire.packet(id=req["id"], part=2, status=200, type="text/csv", body="tail\n", done=True)
        self.assertIsInstance(self.result.get(timeout=1), ConnectionError)

    def test_malformed_json_download_is_not_returned_as_success(self):
        req = self.exchange("/api/v1/export.json")
        self.wire.packet(id=req["id"], part=0, status=200, type="application/json",
                         body='{"meta":{}},"frames":[]}', done=True)
        self.assertIsInstance(self.result.get(timeout=1), ConnectionError)

    def test_disconnect_fails_pending_command_without_retry(self):
        self.exchange("/api/v1/restart", "POST", token="secret")
        self.wire.close()
        self.assertIsInstance(self.result.get(timeout=1), ConnectionError)
        self.assertTrue(self.wire.written.empty())

    def test_timeout_does_not_replay_command(self):
        self.exchange("/api/v1/record", "POST")
        self.assertIsInstance(self.result.get(timeout=2), TimeoutError)
        self.assertTrue(self.wire.written.empty())

    def test_slow_event_subscriber_keeps_recent_data(self):
        with self.bridge.events() as events:
            for seq in range(100):
                self.bridge.publish({"t": "sensors", "seq": seq})
            self.assertLessEqual(events.qsize(), 32)
            last = None
            while not events.empty():
                last = events.get_nowait()
            self.assertEqual(last["seq"], 99)

    def test_request_limits_and_invalid_paths(self):
        for path, method, body in [("/other", "GET", ""), ("/api/v1/config", "DELETE", ""),
                                   ("/api/v1/config", "POST", "x" * 5000)]:
            with self.assertRaises(ValueError):
                self.bridge.request(path, method, body, "")
        self.assertTrue(self.wire.written.empty())


class LiveOnlyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.recordings = Path(self.temp.name) / 'sessions.sqlite3'
        self.original = b'Existing recording fixture: live mode must never open this database.'
        self.recordings.write_bytes(self.original)
        self.wire = Wire()
        self.bridge = SerialBridge('COM7', open_port=lambda port: self.wire)
        self.bridge.start()
        self.addCleanup(self.bridge.close)
        with patch('usb_dashboard.SessionRecorder', side_effect=AssertionError('Live mode accessed recordings')):
            self.server = make_server(self.bridge, 0, self.recordings, live_only=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def request(self, path, method='GET', headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=2)
        try:
            connection.request(method, path, body='{}' if method=='POST' else None, headers=headers or {})
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def test_live_page_and_port_info_do_not_open_recordings(self):
        status, page = self.request('/')
        self.assertEqual(status, 200)
        self.assertIn(b'Receive-only mode', page)
        self.assertNotIn(b'/api/v1/', page)
        status, data = self.request('/usb/info')
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(data), {'port':'COM7', 'baud':921600})
        self.assertFalse(hasattr(self.server, 'sessions'))
        self.assertEqual(self.recordings.read_bytes(), self.original)

    def test_device_changes_and_recording_access_never_reach_serial(self):
        for path in ('/api/v1/config','/api/v1/restart','/api/v1/record','/api/v1/factory_reset','/api/v1/sessions'):
            self.assertEqual(self.request(path, 'POST')[0], 405)
            self.assertEqual(self.request(path)[0], 404)
        self.assertEqual(self.request('/', headers={'Origin':'https://evil.example'})[0], 403)
        self.assertEqual(self.request('/usb/events', headers={'Host':'evil.example'})[0], 403)
        self.assertTrue(self.wire.written.empty())
        self.assertEqual(self.recordings.read_bytes(), self.original)

    def test_live_sensor_event_reaches_browser_without_device_api_requests(self):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=2)
        self.addCleanup(connection.close)
        connection.request('GET', '/usb/events')
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.wire.packet(event={'t':'sensors','seq':42,'s':[{'id':0,'rx':-12.5,'st':'OK'}]})
        while True:
            line = response.readline()
            if line.startswith(b'data: '):
                event = json.loads(line[6:])
                if event.get('t')=='sensors':
                    break
        self.assertEqual(event['seq'], 42)
        self.assertEqual(event['s'][0]['rx'], -12.5)
        while not self.wire.written.empty():
            self.assertEqual(json.loads(self.wire.written.get_nowait()[5:]), {'subscribe':True})
        self.assertEqual(self.recordings.read_bytes(), self.original)


class HttpTests(BridgeTests):
    def setUp(self):
        super().setUp()
        self.sessions_temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.sessions_temp.cleanup)
        self.server = make_server(self.bridge, 0, Path(self.sessions_temp.name) / 'sessions.sqlite3')
        self.http_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.http_thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.port = self.server.server_address[1]

    def get(self, path, headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        self.addCleanup(c.close)
        c.request("GET", path, headers=headers or {})
        r = c.getresponse()
        return r.status, r.read()

    def test_real_dashboard_assets_are_served(self):
        status, body = self.get("/")
        self.assertEqual(status, 200)
        self.assertIn(b"SCRATCHSOR", body)
        self.assertIn(b"app.js", body)

    def test_dashboard_assets_are_not_cached_across_updates(self):
        c = http.client.HTTPConnection('127.0.0.1', self.port, timeout=2)
        self.addCleanup(c.close)
        c.request('GET', '/css/style.css')
        response = c.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader('Cache-Control'), 'no-store')
        response.read()

    def test_foreign_origin_and_rebinding_hosts_are_rejected_before_serial(self):
        for headers in [{"Origin": "https://evil.example"}, {"Host": "evil.example"},
                        {"Referer": "https://evil.example/page"}]:
            self.assertEqual(self.get("/api/v1/device", headers)[0], 403)
        self.assertTrue(self.wire.written.empty())

    def test_api_request_and_download_cross_http_and_serial(self):
        result = queue.Queue()
        t = threading.Thread(target=lambda: result.put(self.get("/api/v1/export.json")))
        t.start()
        req = json.loads(self.wire.written.get(timeout=1)[5:])
        self.wire.packet(id=req["id"], part=0, status=200, type="application/json", body='{"frames":[', done=False)
        self.wire.packet(id=req["id"], part=1, status=200, type="application/json", body="1,2]}", done=True)
        self.assertEqual(result.get(timeout=2), (200, b'{"frames":[1,2]}'))
        t.join(2)

    def test_calibration_csv_download_preserves_signed_labels_and_features(self):
        result = queue.Queue()
        body = 'trial_id,role,reference_cm,category,s1_x\n7,test,-2.5,Below Standard,0.123456791\n'
        def download():
            c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
            try:
                c.request("GET", "/api/v1/calibration.csv")
                r = c.getresponse()
                result.put((r.status, r.getheader("Content-Disposition"), r.read()))
            finally:
                c.close()
        t = threading.Thread(target=download)
        t.start()
        req = json.loads(self.wire.written.get(timeout=1)[5:])
        self.assertEqual(req["path"], "/api/v1/calibration.csv")
        self.wire.packet(id=req["id"], part=0, status=200, type="text/csv", body=body[:40], done=False)
        self.wire.packet(id=req["id"], part=1, status=200, type="text/csv", body=body[40:], done=True)
        self.assertEqual(result.get(timeout=2), (200, 'attachment; filename="calibration.csv"', body.encode()))
        t.join(2)

    def test_live_event_crosses_serial_and_http_eventsource(self):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        self.addCleanup(c.close)
        c.request("GET", "/usb/events")
        response = c.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Content-Type"), "text/event-stream")
        self.wire.packet(event={"t": "sensors", "seq": 123, "s": []})
        for _ in range(6):
            line = response.readline()
            if line.startswith(b"data: "):
                event = json.loads(line[6:])
                if event.get("t") == "sensors":
                    self.assertEqual(event["seq"], 123)
                    break
        else:
            self.fail("sensor event did not reach EventSource")

    def test_windows_browser_abort_closes_event_subscription_cleanly(self):
        class AbortedWriter:
            def write(self, data):
                raise ConnectionAbortedError("Windows browser closed the tab")

        handler = object.__new__(DashboardHandler)
        handler.server = self.server
        handler.wfile = AbortedWriter()
        handler.send_response = lambda *args: None
        handler.send_header = lambda *args: None
        handler.end_headers = lambda: None
        handler._events()
        self.assertFalse(self.bridge.subscribers)

    def test_event_at_header_completion_is_not_lost(self):
        messages = []

        class Writer:
            def write(self, data):
                messages.append(data)
                if b'"seq": 123' in data:
                    raise ConnectionAbortedError("done")
                if len(messages) >= 2:
                    raise ConnectionAbortedError("event was lost")

            def flush(self):
                pass

        handler = object.__new__(DashboardHandler)
        handler.server = self.server
        handler.wfile = Writer()
        handler.send_response = lambda *args: None
        handler.send_header = lambda *args: None
        handler.end_headers = lambda: self.bridge.publish({"t": "sensors", "seq": 123})
        handler._events()
        self.assertTrue(any(b'"seq": 123' in message for message in messages))
        self.assertFalse(self.bridge.subscribers)


if __name__ == "__main__":
    unittest.main()
