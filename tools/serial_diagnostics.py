#!/usr/bin/env python3
"""SCRATCHSOR serial diagnostics - passive monitor + log parser.

The firmware exposes no serial CLI; this tool only listens. It timestamps
every line, colorizes [I]/[W]/[E]/[D] log prefixes, extracts the boot
health-report lines, can write a rotating log file, and prints a summary
(level counts + detected sensor-online count) on Ctrl+C or stream end.

Requires: pyserial (`pip install pyserial`). Everything else is stdlib.

Examples:
    python tools/serial_diagnostics.py --port COM5
    python tools/serial_diagnostics.py --port COM5 --reset --log boot.log
    python tools/serial_diagnostics.py --port COM5 --stats --json stats.json
    python tools/serial_diagnostics.py --replay boot.log          # offline
"""
from __future__ import annotations

import argparse
import json
import logging
import re
import sys
import time
from datetime import datetime
from logging.handlers import RotatingFileHandler

try:
    import serial  # type: ignore
except ImportError:
    serial = None  # --replay still works without pyserial

RESET = "\033[0m"
COLORS = {"[I]": "\033[36m", "[W]": "\033[33m", "[E]": "\033[31m", "[D]": "\033[90m"}

BOOT_RES = [  # boot health-report lines worth extracting
    ("firmware", re.compile(r"SCRATCHSOR firmware\s+(\S+)")),
    ("mux", re.compile(r"PCA9548A found at (0x[0-9A-Fa-f]{2})")),
    ("mux_map", re.compile(r"mux map:\s*(.*)")),
    ("oled", re.compile(r"OLED at (0x[0-9A-Fa-f]{2}) controller=(\d+)")),
    ("sensors", re.compile(r"Magnetometers online:\s*(\d+)/(\d+)")),
    ("ap", re.compile(r"AP SSID (\S+)\s+portal (\S+)")),
]


def ansi_supported(stream) -> bool:
    if not stream.isatty():
        return False
    if sys.platform != "win32":
        return True
    try:  # enable ENABLE_VIRTUAL_TERMINAL_PROCESSING on the console
        import ctypes
        h = ctypes.windll.kernel32.GetStdHandle(-11)
        mode = ctypes.c_uint()
        if ctypes.windll.kernel32.GetConsoleMode(h, ctypes.byref(mode)):
            ctypes.windll.kernel32.SetConsoleMode(h, mode.value | 0x4)
            return True
    except Exception:
        pass
    return False


class Monitor:
    def __init__(self, color: bool, logfile: str | None):
        self.color = color
        self.n = {"lines": 0, "info": 0, "warn": 0, "error": 0, "debug": 0,
                  "other": 0}
        self.boot: dict[str, object] = {}
        self.sensors_online = self.sensors_expected = None
        self.t0 = self.t_last = None
        self.log = None
        if logfile:
            self.log = logging.getLogger("scr.log")
            self.log.setLevel(logging.INFO)
            h = RotatingFileHandler(logfile, maxBytes=1_000_000,
                                    backupCount=3, encoding="utf-8")
            h.setFormatter(logging.Formatter("%(message)s"))
            self.log.addHandler(h)

    def line(self, text: str):
        now = datetime.now()
        ts = now.strftime("%H:%M:%S.") + f"{now.microsecond // 1000:03d}"
        self.t0 = self.t0 or now
        self.t_last = now
        self.n["lines"] += 1
        level = next((k for k in COLORS if text.startswith(k)), None)
        self.n[{"[I]": "info", "[W]": "warn", "[E]": "error",
                "[D]": "debug"}.get(level, "other")] += 1
        for name, rx in BOOT_RES:
            m = rx.search(text)
            if m:
                self.boot[name] = m.groups()
                if name == "sensors":
                    self.sensors_online, self.sensors_expected = (
                        int(m.group(1)), int(m.group(2)))
        print(f"{ts}  {(COLORS[level] + text + RESET) if (level and self.color) else text}")
        if self.log:
            self.log.info(f"{now.isoformat(timespec='milliseconds')}\t{text}")

    def summary(self, extended: bool) -> dict:
        dt = (self.t_last - self.t0).total_seconds() if self.t0 else 0.0
        out = {"lines": self.n["lines"], "errors": self.n["error"],
               "warnings": self.n["warn"], "duration_s": round(dt, 1),
               "sensors_online": self.sensors_online,
               "sensors_expected": self.sensors_expected,
               "boot": {k: list(v) for k, v in self.boot.items()}}
        print("\n--- SCRATCHSOR serial summary " + "-" * 30)
        print(f"lines={out['lines']}  errors={out['errors']}  "
              f"warnings={out['warnings']}  duration={out['duration_s']}s")
        if self.sensors_online is not None:
            print(f"sensors online: {self.sensors_online}/"
                  f"{self.sensors_expected}")
        else:
            print("sensors online: <not seen in this capture>")
        if extended and self.boot:
            for k, v in self.boot.items():
                print(f"  boot.{k}: {' '.join(map(str, v))}")
        return out


def run_stream(stream, mon: Monitor, stats: bool, replay: bool):
    buf = b""
    try:
        while True:
            chunk = stream.read(4096)  # serial: up to timeout; file: to EOF
            if not chunk:
                if replay:
                    break
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                mon.line(raw.decode("utf-8", "replace").rstrip("\r"))
    except KeyboardInterrupt:
        print("\n(interrupted)")
    if buf:
        mon.line(buf.decode("utf-8", "replace").rstrip("\r"))
    return mon.summary(stats)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="serial port, e.g. COM5 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--log", metavar="FILE", help="rotating log file (1 MB x3)")
    ap.add_argument("--json", metavar="FILE", help="write exit summary as JSON")
    ap.add_argument("--stats", action="store_true",
                    help="extended summary incl. extracted boot report")
    ap.add_argument("--reset", action="store_true",
                    help="pulse RTS to reset the board at connect (captures boot)")
    ap.add_argument("--replay", metavar="FILE",
                    help="parse a saved log instead of a serial port")
    ap.add_argument("--no-color", action="store_true")
    a = ap.parse_args()
    if not a.port and not a.replay:
        ap.error("--port or --replay required")

    color = ansi_supported(sys.stdout) and not a.no_color
    mon = Monitor(color, a.log)

    if a.replay:
        stream = open(a.replay, "rb", buffering=0)
    else:
        if serial is None:
            ap.error("pyserial not installed: pip install pyserial")
        try:
            stream = serial.Serial(a.port, a.baud, timeout=0.5)
        except serial.SerialException as e:
            sys.exit(f"cannot open {a.port}: {e}")
        if a.reset:
            stream.setDTR(False); stream.setRTS(True)
            time.sleep(0.1); stream.setRTS(False)

    try:
        result = run_stream(stream, mon, a.stats, replay=bool(a.replay))
    finally:
        stream.close()
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
    return 1 if mon.n["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
