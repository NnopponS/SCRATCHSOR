# USB Dashboard Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans in this chat. Steps use checkbox syntax for tracking.

**Goal:** Full dashboard access over the existing USB serial cable.

**Architecture:** A WebServer adapter registers each existing API handler for both HTTP and serial requests. A loopback Python server serves `data/`, forwards API requests, and delivers the existing live-event JSON through EventSource. Wi-Fi keeps its existing WebSocket path.

**Tech Stack:** Arduino/ESP32, ArduinoJson, Python standard library plus pyserial, existing vanilla JavaScript.

**Spec:** `docs/superpowers/specs/2026-11-03-usb-dashboard-design.md`

## Global Constraints

- Preserve existing handlers, token validation, stored calibration/configuration, and Wi-Fi access.
- Bind to `127.0.0.1`; validate browser Host and Origin/Referer before serial access.
- Bound serial input/output messages and response accumulation; use timeouts and explicit errors. Never replay a mutating request automatically after a disconnect.
- Keep 115200 baud; live streaming expires without bridge heartbeats.
- No erase-flash operation. Hardware verification requires a detected serial device.

## Review Focus

- Partial/oversized serial lines must recover at the next delimiter without executing fragments.
- Foreign browser origins and DNS-rebinding Host headers must not reach the device.
- Disconnect/timeout must fail pending requests, never report successful commands or retry writes.
- CSV/JSON export chunks must be assembled exactly, including non-ASCII text and final delimiters.
- A slow browser must not block firmware event consumption or other clients.

## Task 1: Shared USB firmware transport

Files: `include/networking/usb_protocol.h`, `include/networking/dashboard_transport.h`, `src/networking/dashboard_transport.cpp`, `include/networking/web_server.h`, `src/networking/websocket_server.cpp`, `test/test_usb_protocol/test_main.cpp`.

Interface: bounded newline JSON packets prefixed `@USB `; request `{id,method,path,token,body}`; response `{id,status,type,body,done}`; live `{event:{t,...}}`; heartbeat `{subscribe:true}`. Requests are serialized by the bridge. Bodies remain strings to reuse existing handlers. Response chunks are at most 512 bytes of body, UTF-8 preserved by splitting only at character boundaries. The adapter must route only registered API methods/paths and preserve token checks.

- [x] Add bounded-line/recovery tests. Initial native execution was blocked by a missing compiler; installed the PlatformIO host toolchain and ran the tests.
- [x] Implement the line parser and WebServer adapter; share route callbacks and preserve all HTTP calls.
- [x] Mirror existing events only while subscribed, with a five-second expiry and a lower USB sensor rate to fit 115200 baud.
- [x] Build firmware and run host tests.

## Task 2: Loopback bridge and dashboard integration

Files: `tools/usb_dashboard.py`, `tools/test_usb_dashboard.py`, `data/js/app.js`, `Start USB Dashboard.cmd`, `docs/usb-dashboard.md`, `README.md`.

Interface: `python tools/usb_dashboard.py --port COM7`; optional auto-detection when exactly one serial port exists. HTTP `/api/v1/*` maps to serial packets. `/usb/events` is EventSource using the same event objects as Wi-Fi WebSockets. The bridge reconnects its serial reader; it never resends failed API calls.

- [x] Write failing stdlib unittest coverage for framing, errors, response chunks, browser-origin checks, and bounded live queues.
- [x] Implement bridge with request serialization, bounded responses/queues, port selection, finite serial timeouts, and Ctrl+C cleanup.
- [x] Enable EventSource for loopback-served dashboard; preserve Wi-Fi boot behavior and mark disconnection/stale data.
- [x] Add a launcher and instructions; explain flashing without erase and serial-port exclusivity.
- [x] Run Python tests, native tests, firmware/filesystem build, and browser smoke test. Report hardware limitations accurately.

## Execution ledger

- Design approved by user. Execute here without another approval gate.
- Workspace has no `.git`; edit in place and retain backups of touched existing files under `.pio/bench-backup/usb-dashboard/`. No commits/worktrees can be created here.
- The `graft` command is not installed in the shell; use source reads and `rg` as fallback.
- Review fixed the default 256-byte serial receive limit by configuring 8192 bytes before Serial.begin. USB messages stay bounded at 4096 bytes.
- Review found an existing JSON-export root-brace error; fixed the shared handler for both transports. Bridge response-chunk sequencing and JSON validation prevent incomplete downloads being returned as success.
- Browser smoke used an isolated controlled serial peer: all seven views, live updates, recording command, and JSON download worked with no console errors. Closing the browser exposed Windows ConnectionAbortedError; reproduced in a regression test and handled with the socket ConnectionError family.
- Physical upload, real USB timing/reconnect, and real Wi-Fi regression testing remain unverified: no serial port is detected. No board was flashed or erased.
