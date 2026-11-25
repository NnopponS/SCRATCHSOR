# SCRATCHSOR Wi-Fi guide

`WifiManager` runs SoftAP + STA concurrently (mode `AP_STA` by default).
The AP is your always-available management interface; STA is optional and
joins your LAN for convenience. Design rule: **the device can never strand
you** — if STA fails, the AP is still there.

## 1. Modes

| Mode | AP | STA | When |
|---|---|---|---|
| `AP_ONLY` | up (`SCRATCHSOR-XXXX`, 192.168.4.1) | off | factory state / no stored creds |
| `AP_STA` | up | connecting/connected | normal after provisioning |
| `STA_ONLY` | down | connected | only if you disable AP persistence |

`WifiMode` enum: `device_types.h`. Default config `apPersistent=true` —
the AP stays up even after STA joins, because it's your recovery path.

## 2. Identity and password

- **SSID:** `SCRATCHSOR-XXXX` where XXXX = last 4 hex digits of the efuse
  MAC — unique per device, stable across reboots.
- **AP password:** identical to the SSID, as requested for this bench firmware.
  On the connected board both are `SCRATCHSOR-9BD8`. The previous generated
  NVS password is not used by this build.
- **Admin token:** the same SSID string, entered in the dashboard when needed.
- **OLED:** shows profile, categorical result and four monochrome sensor dots only.
  Filled means online; hollow means offline. Connection details are here and in
  the browser. Find `SCRATCHSOR-XXXX` in your Wi-Fi list; its name is also its
  password and dashboard admin token. Open `http://192.168.4.1` after joining.

## 3. Captive portal

While the AP is up, `CaptivePortal` (DNS :53) wildcard-resolves every name
to the AP IP, and the HTTP server answers the usual OS probe paths
(`/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`, `/connecttest.txt`,
`/redirect`) with the portal page — so most devices auto-open the dashboard
after joining.

**OS variance (expected, not a bug):**

- Android usually pops a "Sign in to network" banner; some builds need a
  tap on the notification.
- iOS opens the Captive Network Assistant sheet; occasionally it probes a
  path that lands on 404 → the sheet shows a blank page. Close it and use
  Safari.
- Windows may only mark the network "no internet" and not auto-open
  anything.
- Some OSes disengage data on a no-internet network — you may need to
  "keep Wi-Fi anyway" on Android.

**Universal fallback: browse to `http://192.168.4.1`.** That address is
fixed; portal or not, it always reaches the dashboard while you're on the
AP.

## 4. AP+STA channel sharing — read this

A single radio serves both interfaces. The AP's channel is set at
`kApChannel` = 1 **only while STA is disconnected**. Once STA associates,
the radio must follow the infrastructure channel: **the AP silently moves
to whatever channel your router uses.** Consequences:

- A client already on the AP may briefly drop when STA connects (channel
  hop). Reconnect — same SSID/password.
- Dashboard on the AP IP can stall for a second during the hop; WS
  reconnects itself.
- If your router is on a crowded channel, AP-side latency inherits that
  congestion.
- During bounded STA connect attempts (`kStaConnectTimeoutMs` = 15 s) and
  the 30 s retry backoff, the AP returns to ch1 — so AP reliability dips
  while STA retries are happening. This is normal.

## 5. Provisioning flow (join your LAN)

1. Connect to `SCRATCHSOR-XXXX`, open `http://192.168.4.1`.
2. Dashboard → Wi-Fi Settings → **Scan** (`GET /api/v1/wifi/scan` —
   `done:false` means still scanning; poll again).
3. Pick the SSID (open networks flagged), enter password → **Connect**
   (`POST /api/v1/wifi/connect`, requires the admin token header).
4. Watch `GET /api/v1/wifi/status`: `sta.state` goes
   CONNECTING → CONNECTED with `sta.ip` + `rssi`, or FAILED with a reason
   string (`sta.error`).
5. On success the firmware persists creds in NVS and reconnects on every
   boot — before the AP finishes coming up, typically within a few
   seconds.

Failed attempts automatically back off (`kStaRetryBackoffMs` = 30 s) and
retry — wrong passwords surface as `sta.error` ≈ "auth failed" /
"no AP found" reason strings, never as silent hangs.

## 6. Forget / reset

- **Forget this network:** `POST /api/v1/wifi/forget` → disconnects STA and
  clears stored SSID/password. AP untouched. (Dashboard button does this.)
- **Regenerate AP password:** changes both the AP password and the admin
  token; shown on the OLED after regen. Existing clients must rejoin.
- **Factory reset:** `POST /api/v1/factory_reset` with `{"keepWifi":true}`
  keeps Wi-Fi creds and wipes everything else; `keepWifi:false` wipes all
  including the stored STA credentials and AP password (a new one is
  generated next boot — OLED shows it).
- **Full wipe without API access:** `pio run -t erase` erases flash
  (firmware + NVS + LittleFS); then reflash firmware + fs. The device
  comes back as new.

## 7. Security notes

- Scope: this is a **bench/local-network device**. AP WPA2 with a random
  per-device password; admin token = that password, sent as
  `X-SCRATCHSOR-Token` header on mutating `POST`s; `Origin`/`Referer`
  checked (same-origin or absent). There is no TLS — the token traverses
  the local link in plaintext. Don't expose the device to untrusted
  networks, and don't reuse the AP password anywhere else.
- GETs are intentionally open on the device's own network (the dashboard
  needs them); only mutations need the token.
- The stored STA password is write-only: readable by nothing over the
  network and never logged. If you forget it, re-entering it in Settings
  overwrites it.
- Anyone with physical OLED access can read the AP password — that's by
  design for a portable instrument. Don't leave it unattended on
  networks you care about.
- `apPersistent=true` is the safe default. Setting it false removes your
  recovery interface; only do it after STA connectivity is proven stable.

## 8. Reference

- Status JSON shape: `GET /api/v1/wifi/status` →
  `{"mode":"AP_STA","sta":{...},"ap":{"ssid","ip","clients"}}`
  (see `docs/api.md`).
- WS `status` frames push the same data every ~2 s and on change.
- Constants: `include/config.h` (`kApChannel`, `kApMaxClients`,
  `kStaConnectTimeoutMs`, `kStaRetryBackoffMs`, `kHttpPort` 80,
  `kWsPort` 81, `kDnsPort` 53, `kApPasswordLen` 10).
