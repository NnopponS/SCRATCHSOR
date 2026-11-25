# Full dashboard over USB

The PC will serve the existing dashboard at `http://127.0.0.1:8765`. A small Python bridge will exchange requests and live events with the ESP32 over its USB serial port. The PC can stay on its normal network; no connection to the ESP32 Wi-Fi is required.

## Approaches

- **Local bridge (selected by the user):** reuse the dashboard and firmware API handlers; one local Python process and pyserial are required.
- **Direct browser serial:** avoids the local bridge but needs browser-specific serial access and more changes to requests and file downloads. Not selected.

## Scope and behavior

- Support live readings, health, settings, calibration, recording, CSV/JSON downloads, and the existing restart/reset controls.
- Share existing firmware handlers between HTTP and USB so validation and admin-token checks remain consistent.
- Serve dashboard assets from the project on the PC. Retain existing Wi-Fi access.
- Bind the bridge only to loopback and reject requests from other web origins. Bound serial messages, handle timeouts/disconnects, and keep logs separate from protocol frames.
- Keep existing serial speed at 115200. Only stream when a bridge is present; keep acquisition running independently.
- Provide one documented launch command and an optional Windows launcher. Detect the serial port when unambiguous; allow an explicit port otherwise.

## Verification and limits

Test request/response framing, authentication preservation, streamed downloads, local HTTP/event delivery, and disconnect behavior. Build firmware and run existing host checks where tooling is available.

Flashing preserves calibration and saved configuration; do not erase flash. Actual flashing and USB verification require a recognized board. This PC currently reports no serial port. Hardware performance and reconnection cannot be claimed until checked on the device.

Implementation and verification will be done in this chat. No UI redesign or additional device features are included.
