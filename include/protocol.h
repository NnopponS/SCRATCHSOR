#pragma once
// Wire contract between firmware and the embedded Web UI.
// Both the REST/WS implementation (src/networking) and the frontend (data/)
// MUST conform to this file. Field names below are authoritative.
// See docs/api.md for full endpoint documentation and examples.

#include <cstdint>

namespace scratchsor {
namespace proto {

// ---- WebSocket (ws://<device>:81/ws) ----
// Server pushes JSON text frames; compact but self-describing.

// Message type tags (field "t"):
inline constexpr const char* kMsgHello   = "hello";    // on connect
inline constexpr const char* kMsgSensors = "sensors";  // ~10 Hz live frame
inline constexpr const char* kMsgStatus  = "status";   // wifi/health/measurement
inline constexpr const char* kMsgCalib   = "calib";    // calibration progress
inline constexpr const char* kMsgRecord  = "record";   // recorder state
inline constexpr const char* kMsgError   = "error";

// --- sensors frame ---
// {"t":"sensors","seq":123,"ts":4567,"rate":9.8,
//  "s":[{"id":0,"x":12.3,"y":-4.5,"z":98.7,"m":99.7,
//        "rx":13.0,"ry":-5.0,"rz":99.0,"rm":99.98,
//        "st":"OK","sat":false,"q":0.92}, ...x4],
//  "est":{"state":"VALID","category":"Standard","profile":"Male / Right on top",
//         "profileId":0,"selected":true,"ready":true,"conf":1,"ts":4567}}
// Fields x/y/z/m are filtered microtesla; rx/ry/rz/rm are raw Bx/By/Bz/|B|.
// "st" = toString(SensorStatus).

// --- status frame (sent on change + every ~2 s heartbeat) ---
// {"t":"status","up":12345,"heap":180000,
//  "wifi":{"mode":"AP_STA","sta":"CONNECTED","staSsid":"...","staIp":"...",
//          "rssi":-52,"apSsid":"SCRATCHSOR-1A2B","apIp":"192.168.4.1",
//          "apClients":1},
//  "health":{"mux":true,"oled":true,"rate":9.9,
//            "sensors":["OK","OK","TIMEOUT","OK"]}}

// --- calib frame ---
// {"t":"calib","phase":"baseline|collect|fit|done|error",
//  "refMm":-25.0,"collected":10,"target":20,"progress":0.5,"detail":"..."}

// --- record frame ---
// {"t":"record","active":true,"count":10,"max":128,"durationMs":3000,
//  "remainingMs":2000,"refMm":50,"startedMs":12345}

// Client -> server messages (JSON text):
// {"t":"ping"}                      -> {"t":"pong","ts":...}
// {"t":"subscribe","ch":["sensors","status"]}   (optional filtering)
// {"t":"record","active":true,"refMm":50,"token":"..."} (see api.md)

// ---- REST ----
// All endpoints are prefixed /api/v1. Mutating POSTs require header:
//   X-SCRATCHSOR-Token: <admin token>   (admin token = device AP password)
// plus a permissible Origin/Referer (same-host or absent). 401/403 on failure.
// Error body: {"error":"short_code","detail":"human readable"}

}  // namespace proto
}  // namespace scratchsor
