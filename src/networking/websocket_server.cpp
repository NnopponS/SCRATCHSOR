// WebSocket server (links2004/WebSockets) on port kWsPort — the frontend
// connects to ws://<host>:81/ws. Frames are serialized once into a shared
// static buffer and broadcast; the library's heartbeat ping/pong reaps dead
// clients. All producers run from the app loop, so the shared buffer needs
// no locking.
//
// Client -> server commands (JSON text):
//   {"t":"ping"}                                -> {"t":"pong","ts":..}
//   {"t":"record","active":true,"token":".."}   -> toggles the recorder
//   {"t":"subscribe","ch":[..]}                 -> accepted, no filtering
// When onCommand is set it receives every text payload instead of the
// built-in handlers.

#include <Arduino.h>
// See wifi_manager.cpp — DISABLED/ENABLED macros vs device_types.h enums.
#undef DISABLED
#undef ENABLED
#include "networking/websocket_server.h"
#include "networking/wifi_manager.h"
#include "networking/usb_protocol.h"
#include "services/sensor_service.h"
#include "services/calibration_service.h"
#include "diagnostics/diagnostics.h"
#include "protocol.h"

#include <WiFi.h>
#include <ArduinoJson.h>
#include <Esp.h>
#include <cstring>

namespace scratchsor {

WsServer gWs;

namespace {

// Shared serialize target for every broadcast — the app loop is
// single-threaded, so one buffer serves all producers.
char sBuf[kWsJsonBufBytes];

// Set only while handling a CONNECT event so sendHello() can address the
// joining client rather than broadcasting to everyone.
int8_t sHelloClient = -1;

constexpr uint32_t kStatusIntervalMs = 2000;  // status heartbeat

const char* wifiModeStr(WifiMode m) {
  switch (m) {
    case WifiMode::STA_ONLY: return "STA";
    case WifiMode::AP_STA:   return "AP_STA";
    default:                 return "AP";
  }
}

}  // namespace

void WsServer::begin() {
  ws_.begin();
  ws_.onEvent([this](uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
    onEvent(num, type, payload, len);
  });
  // Library-managed ping/pong: drop clients that miss two consecutive pongs.
  ws_.enableHeartbeat(15000 /*ping ms*/, 3000 /*pong timeout ms*/, 2);

  // Instant status push on Wi-Fi transitions; the 2 s heartbeat below
  // covers heap/health drift. (wifi_/health_ member pointers stay unused —
  // live values are pulled fresh since nothing else publishes them.)
  gWifi.onStatusChange = [this](const WifiStatus& w) {
    HealthReport h{};
    if (gpSensors) gpSensors->health(h);
    h.uptimeMs = millis();
    if (!h.freeHeap) h.freeHeap = ESP.getFreeHeap();
    broadcastStatus(w, h);
  };
  SCR_LOGI("WS server listening on :%u/ws", kWsPort);
}

void WsServer::update() {
  ws_.loop();
  const uint32_t now = millis();
  if ((clients_ || usbSubscribed()) && now - lastStatusMs_ >= kStatusIntervalMs) {
    lastStatusMs_ = now;
    WifiStatus w = gWifi.status();
    HealthReport h{};
    if (gpSensors) gpSensors->health(h);
    h.uptimeMs = now;
    if (!h.freeHeap) h.freeHeap = ESP.getFreeHeap();
    broadcastStatus(w, h);
  }
}

void WsServer::onEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      if (clients_ < 0xFF) ++clients_;
      sHelloClient = (int8_t)num;
      sendHello();
      sHelloClient = -1;
      break;

    case WStype_DISCONNECTED:
      if (clients_) --clients_;
      break;

    case WStype_TEXT: {
      // Payload is not NUL-terminated — copy into a bounded buffer first.
      char msg[192];
      const size_t n = len < sizeof(msg) - 1 ? len : sizeof(msg) - 1;
      memcpy(msg, payload, n);
      msg[n] = '\0';
      if (onCommand) {
        onCommand(num, msg);
        break;
      }
      JsonDocument doc;
      if (deserializeJson(doc, msg)) break;
      const char* t = doc["t"];
      if (!t) break;

      if (!strcmp(t, "ping")) {
        JsonDocument r;
        r["t"] = "pong";
        r["ts"] = millis();
        char out[80];
        const size_t m = serializeJson(r, out, sizeof(out));
        if (m) ws_.sendTXT(num, out, m);
      } else if (!strcmp(t, "record")) {
        broadcastError("pc_capture_only", "Recording is handled by the PC USB receiver");
        break;
        if (!authorized(msg)) {
          JsonDocument r;
          r["t"] = proto::kMsgError;
          r["code"] = "unauthorized";
          r["detail"] = "bad or missing token";
          char out[96];
          const size_t m = serializeJson(r, out, sizeof(out));
          if (m) ws_.sendTXT(num, out, m);
          break;
        }
        if (!doc["active"].is<bool>()) { broadcastError("bad_request", "Boolean active required");break; }
        const bool active = doc["active"].as<bool>();
        if (gpSensors) {
          if (active) {
            if (!doc["refMm"].is<float>() || !referenceAllowed(doc["refMm"].as<float>())) {
              broadcastError("bad_request", "Measured refMm (-500..500) required");break;
            }
            if (gpCalib && gpCalib->busy()) {broadcastError("busy", "Cancel calibration before recording");break;}
            if (!gpSensors->recordStart(doc["refMm"].as<float>())) {
              broadcastError("invalid_state", "Recorder busy, unavailable, or no fresh healthy sensor readings");break;
            }
          } else gpSensors->recordStop();
        }
        broadcastRecord(gpSensors && gpSensors->recording(),
                        gpSensors ? gpSensors->recordCount() : 0,
                        kRecordMaxFrames);
      }
      // "subscribe": protocol marks channel filtering optional — accepted
      // silently; every client receives all channels.
      break;
    }

    default:
      break;  // BIN/fragment/PING/PONG — nothing to do
  }
}

bool WsServer::authorized(const char* json) const {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  const char* tok = doc["token"];
  return tok && adminToken_.length() && adminToken_ == tok;
}

void WsServer::sendHello() {
  JsonDocument doc;
  doc["t"] = proto::kMsgHello;
  const String name = WiFi.softAPSSID();
  doc["name"] = name.length() ? name : String(kProductName);
  doc["fw"] = kFirmwareVer;
  doc["api"] = kApiVersion;
  char buf[160];
  const size_t n = serializeJson(doc, buf, sizeof(buf));
  if (!n) return;
  if (sHelloClient >= 0) ws_.sendTXT((uint8_t)sHelloClient, buf, n);
  else if (clients_)    ws_.broadcastTXT(buf, n);
}

void WsServer::broadcastSensors(const SensorFrame& f, const ClassificationEstimate& e,
                                float rateHz) {
  static uint32_t lastUsbMs = 0;
  const uint32_t now = millis();
  const bool usbDue = usbSubscribed() && now - lastUsbMs >= kWsSensorIntervalMs;
  const bool wsDue = clients_ && (!lastSensorWsMs_ || now-lastSensorWsMs_ >= kWsUiSensorIntervalMs);
  if (!wsDue && !usbDue) return;
  JsonDocument doc;
  doc["t"] = proto::kMsgSensors;
  doc["seq"] = f.seq;
  doc["ts"] = f.timestampMs;
  doc["rate"] = rateHz;
  JsonArray s = doc["s"].to<JsonArray>();
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    const SensorReading& r = f.sensors[i];
    JsonObject o = s.add<JsonObject>();
    o["id"] = i;
    o["x"] = r.filtered.x;
    o["y"] = r.filtered.y;
    o["z"] = r.filtered.z;
    o["m"] = r.magnitudeFiltered;
    o["rx"] = r.raw.x;
    o["ry"] = r.raw.y;
    o["rz"] = r.raw.z;
    o["rm"] = r.magnitudeRaw;
    o["st"] = millis()-r.timestampMs>1000?"TIMEOUT":toString(r.status);
    o["sat"] = r.saturated;
    o["q"] = r.quality;
  }
  JsonObject est = doc["est"].to<JsonObject>();
  est["state"] = toString(e.state);
  est["category"] = e.category>=0?categoryName(e.category):nullptr;
  est["profile"] = e.selected?profileName(e.profile):"Select profile";
  est["profileId"]=e.profile;est["selected"]=e.selected;est["ready"]=e.ready;est["ts"]=e.timestampMs;
  est["conf"] = e.confidence;
  const size_t n = serializeJson(doc, sBuf, sizeof(sBuf));
  if (n && n < sizeof(sBuf)) {
    if (wsDue) { ws_.broadcastTXT(sBuf, n); lastSensorWsMs_=now; }
    if (usbDue) { usbBroadcast(sBuf, n); lastUsbMs = now; }
  }
}

void WsServer::broadcastStatus(const WifiStatus& w, const HealthReport& h) {
  if (!clients_ && !usbSubscribed()) return;
  JsonDocument doc;
  doc["t"] = proto::kMsgStatus;
  doc["up"] = h.uptimeMs;
  doc["heap"] = h.freeHeap;
  JsonObject wf = doc["wifi"].to<JsonObject>();
  wf["mode"] = wifiModeStr(w.mode);
  wf["sta"] = toString(w.staState);
  wf["staSsid"] = w.staSsid;
  wf["staIp"] = w.staIp.toString();
  wf["rssi"] = w.staRssi;
  wf["apSsid"] = w.apSsid;
  wf["apIp"] = w.apIp.toString();
  wf["apClients"] = w.apClients;
  JsonObject hl = doc["health"].to<JsonObject>();
  hl["mux"] = h.muxFound;
  hl["oled"] = h.oledFound;
  hl["rate"] = h.measuredRateHz;
  JsonArray sa = hl["sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < kSensorCount; ++i) sa.add(toString(h.sensor[i].status));
  const size_t n = serializeJson(doc, sBuf, sizeof(sBuf));
  if (n && n < sizeof(sBuf)) {
    if (clients_) ws_.broadcastTXT(sBuf, n);
    usbBroadcast(sBuf, n);
  }
}

void WsServer::broadcastCalib(const char* phase, float refMm,
                              uint16_t collected, uint16_t target,
                              const char* detail) {
  if (!clients_ && !usbSubscribed()) return;
  JsonDocument doc;
  doc["t"] = proto::kMsgCalib;
  doc["phase"] = phase ? phase : "";
  doc["refMm"] = refMm;
  doc["collected"] = collected;
  doc["target"] = target;
  doc["progress"] = target ? (float)collected / (float)target : 0.0f;
  doc["detail"] = detail ? detail : "";
  const size_t n = serializeJson(doc, sBuf, sizeof(sBuf));
  if (n && n < sizeof(sBuf)) {
    if (clients_) ws_.broadcastTXT(sBuf, n);
    usbBroadcast(sBuf, n);
  }
}

void WsServer::broadcastRecord(bool active, uint16_t count, uint16_t max) {
  if (!clients_ && !usbSubscribed()) return;
  JsonDocument doc;
  doc["t"] = proto::kMsgRecord;
  doc["active"] = active;
  doc["count"] = count;
  doc["max"] = max;
  doc["durationMs"] = kRecordDurationMs;
  doc["remainingMs"] = gpSensors ? gpSensors->recordRemainingMs() : 0;
  doc["refMm"] = gpSensors ? gpSensors->recordReferenceMm() : 0;
  doc["startedMs"] = gpSensors ? gpSensors->recordStartedMs() : 0;
  const size_t n = serializeJson(doc, sBuf, sizeof(sBuf));
  if (n && n < sizeof(sBuf)) {
    if (clients_) ws_.broadcastTXT(sBuf, n);
    usbBroadcast(sBuf, n);
  }
}

void WsServer::broadcastError(const char* code, const char* detail) {
  if (!clients_ && !usbSubscribed()) return;
  JsonDocument doc;
  doc["t"] = proto::kMsgError;
  doc["code"] = code ? code : "";
  doc["detail"] = detail ? detail : "";
  const size_t n = serializeJson(doc, sBuf, sizeof(sBuf));
  if (n && n < sizeof(sBuf)) {
    if (clients_) ws_.broadcastTXT(sBuf, n);
    usbBroadcast(sBuf, n);
  }
}

}  // namespace scratchsor
