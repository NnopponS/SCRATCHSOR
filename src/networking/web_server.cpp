// HTTP server: LittleFS static assets + /api/v1 REST + captive-portal probes.
// Uses the blocking ESP32-core WebServer; every handler is short and only
// touches service snapshots — nothing waits on sensor hardware.
//
// Security (see protocol.h):
//   - every POST requires header X-SCRATCHSOR-Token == adminToken (AP password)
//   - a present Origin/Referer must name the request Host or a device IP,
//     else 403 forbidden_origin; bad/missing token -> 401 unauthorized
//   - GETs are open on the device's own network; the STA password is never
//     serialized anywhere in this file.

#include <Arduino.h>
// See wifi_manager.cpp — DISABLED/ENABLED macros vs device_types.h enums.
#undef DISABLED
#undef ENABLED
#include "networking/web_server.h"
#include "networking/wifi_manager.h"
#include "networking/websocket_server.h"
#include "services/sensor_service.h"
#include "services/calibration_service.h"
#include "services/measurement_service.h"
#include "services/storage_service.h"
#include "hardware/oled_manager.h"
#include "diagnostics/diagnostics.h"
#include "protocol.h"
#include "product/phone_product.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Esp.h>
#include <U8g2lib.h>  // completes U8G2 for oled_manager.h's unique_ptr member
#include <cstdio>
#include <cstring>
#include <cmath>

namespace scratchsor {

ScratchsorWebServer gHttp;

namespace {

constexpr char kAuthHeaderName[] = "X-SCRATCHSOR-Token";

uint32_t sRestartAtMs = 0;  // deferred ESP.restart() — response flushes first
bool     sScanArmed   = false;  // /wifi/scan trigger+read handshake

// ---- small string maps ----------------------------------------------------

const char* wifiModeStr(WifiMode m) {
  switch (m) {
    case WifiMode::STA_ONLY: return "STA";
    case WifiMode::AP_STA:   return "AP_STA";
    default:                 return "AP";
  }
}
const char* calibTypeStr(CalibModelType t) {
  switch (t) {
    case CalibModelType::DIPOLE_LINEAR: return "DIPOLE_LINEAR";
    case CalibModelType::DIPOLE_QUAD:   return "DIPOLE_QUAD";
    default:                            return "NONE";
  }
}
const char* oledCtrlStr(OledController c) {
  switch (c) {
    case OledController::SSD1306: return "SSD1306";
    case OledController::SH1106:  return "SH1106";
    default:                      return "UNKNOWN";
  }
}

const char* contentTypeFor(const String& p) {
  if (p.endsWith(".html") || p.endsWith(".htm")) return "text/html";
  if (p.endsWith(".css"))  return "text/css";
  if (p.endsWith(".js"))   return "application/javascript";
  if (p.endsWith(".svg"))  return "image/svg+xml";
  if (p.endsWith(".png"))  return "image/png";
  if (p.endsWith(".ico"))  return "image/x-icon";
  if (p.endsWith(".json")) return "application/json";
  if (p.endsWith(".txt"))  return "text/plain";
  return "application/octet-stream";
}

// Same-origin check: extract host[:port] from an Origin/Referer value and
// compare to the request Host or the device's own IPs. Bare IPs/hosts
// (no scheme) are accepted the same way.
bool refHostAllowed(const String& ref, const String& hostHdr) {
  String h = ref;
  const int scheme = h.indexOf("://");
  if (scheme >= 0) h = h.substring(scheme + 3);
  const int slash = h.indexOf('/');
  if (slash >= 0) h = h.substring(0, slash);
  const int at = h.lastIndexOf('@');          // drop userinfo if present
  if (at >= 0) h = h.substring(at + 1);
  const int colon = h.indexOf(':');
  if (colon >= 0) h = h.substring(0, colon);  // drop :port
  if (!h.length()) return true;
  String host = hostHdr;
  const int hc = host.indexOf(':');
  if (hc >= 0) host = host.substring(0, hc);
  if (h == host) return true;
  if (h == WiFi.softAPIP().toString()) return true;
  const IPAddress lip = WiFi.localIP();
  if (lip != IPAddress((uint32_t)0) && h == lip.toString()) return true;
  return false;
}

// ---- JSON serializers (field names are contractual — see api.md) ----------

void configToJson(JsonObject o, const DeviceConfig& c) {
  o["deviceName"] = c.deviceName;
  o["apPersistent"] = c.apPersistent;
  JsonObject s = o["sensor"].to<JsonObject>();
  s["gain"] = c.sensor.gain;         s["resolution"] = c.sensor.resolution;
  s["osr"] = c.sensor.oversampling;  s["digFilt"] = c.sensor.digitalFilter;
  s["rateHz"] = c.sensor.sampleRateHz; s["temp"] = c.sensor.readTemperature;
  JsonObject f = o["filter"].to<JsonObject>();
  f["avg"] = c.filter.movingAvgEnabled;    f["avgWin"] = c.filter.movingAvgWindow;
  f["median"] = c.filter.medianEnabled;    f["medWin"] = c.filter.medianWindow;
  f["lp"] = c.filter.lowpassEnabled;       f["alpha"] = c.filter.lowpassAlpha;
  f["baseline"] = c.filter.baselineRemoval; f["outlierSigma"] = c.filter.outlierSigma;
  JsonObject d = o["oled"].to<JsonObject>();
  d["refreshHz"] = c.oled.refreshHz;  d["rotateSec"] = c.oled.rotateSec;
  d["flip"] = c.oled.flipDisplay;     d["controller"] = c.oled.controllerOverride;
  JsonObject reach=o["reach"].to<JsonObject>();
  reach["profile"]=c.reach.profile;reach["selected"]=c.reach.selected;reach["setup"]=c.reach.setup;
  JsonObject g = o["gap"].to<JsonObject>();
  g["enabled"] = c.gap.enabled; g["fixedGeometry"] = c.gap.fixedGeometry;
  g["sensorId"] = c.gap.sensorId; g["reference"] = c.gap.reference;
  g["setup"] = c.gap.setup;
}

void readingToJson(JsonObject o, const SensorReading& r, uint8_t id, bool raw) {
  o["id"] = id;
  o["x"] = r.filtered.x; o["y"] = r.filtered.y; o["z"] = r.filtered.z;
  o["m"] = r.magnitudeFiltered;
  if (raw) {
    o["rx"] = r.raw.x; o["ry"] = r.raw.y; o["rz"] = r.raw.z;
    o["rm"] = r.magnitudeRaw;
  }
  o["st"] = millis()-r.timestampMs>1000?"TIMEOUT":toString(r.status);
  o["sat"] = r.saturated;
  o["q"] = r.quality;
}

void wifiStatusToJson(JsonObject o, const WifiStatus& w) {
  o["mode"] = wifiModeStr(w.mode);
  JsonObject sta = o["sta"].to<JsonObject>();
  sta["state"] = toString(w.staState);
  sta["ssid"] = w.staSsid;
  sta["ip"] = w.staIp.toString();
  sta["rssi"] = w.staRssi;
  sta["error"] = w.staError;
  JsonObject ap = o["ap"].to<JsonObject>();
  ap["ssid"] = w.apSsid;
  ap["ip"] = w.apIp.toString();
  ap["clients"] = w.apClients;
}

void calibModelToJson(JsonObject o, const FingerprintModel& m) {
  o["type"]="KNN3";o["ready"]=m.ready;o["validated"]=m.ready;o["accuracy"]=m.accuracy;
  JsonArray train=o["trainCount"].to<JsonArray>(), test=o["testCount"].to<JsonArray>();
  JsonArray recall=o["recall"].to<JsonArray>(), confusion=o["confusion"].to<JsonArray>();
  for(unsigned c=0;c<3;++c){train.add(m.trainCount[c]);test.add(m.testCount[c]);recall.add(m.recall[c]);
    auto row=confusion.add<JsonArray>();for(unsigned j=0;j<3;++j)row.add(m.confusion[c][j]);}
}
void classificationToJson(JsonObject o,const ClassificationEstimate& e) {
  o["state"]=toString(e.state);o["category"]=e.category>=0?categoryName(e.category):nullptr;
  o["profile"]=e.selected?profileName(e.profile):"Select profile";
  o["profileId"]=e.profile;o["selected"]=e.selected;o["ready"]=e.ready;
  o["conf"]=e.confidence;o["ts"]=e.timestampMs;
}

template <typename T>
T clampT(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// ---- lifecycle ------------------------------------------------------------

void ScratchsorWebServer::begin(const Services& services, const String& adminToken) {
  svc_ = services;
  adminToken_ = adminToken;
  // Only listed headers are collected; Host is always parsed.
  static const char* kHeaders[] = {kAuthHeaderName, "Origin", "Referer"};
  server_.collectHeaders(kHeaders, 3);
  registerRoutes();
  server_.begin();
  SCR_LOGI("HTTP server listening on :%u", kHttpPort);
}

void ScratchsorWebServer::update() {
  server_.handleClient();
  if (sRestartAtMs && (int32_t)(millis() - sRestartAtMs) >= 0) {
    sRestartAtMs = 0;
    SCR_LOGI("restarting (deferred)");
    delay(50);            // let the TX FIFO + TCP stack drain the response
    ESP.restart();
  }
}

// ---- plumbing -------------------------------------------------------------

void ScratchsorWebServer::sendJson(int code, const char* json) {
  server_.send(code, "application/json", json);
}

void ScratchsorWebServer::sendError(int code, const char* err, const char* detail) {
  JsonDocument doc;
  doc["error"] = err;
  doc["detail"] = detail ? detail : "";
  String out;
  serializeJson(doc, out);
  sendJson(code, out.c_str());
}

bool ScratchsorWebServer::requireAuth() {
  const String host = server_.hostHeader();
  if (server_.hasHeader("Origin") &&
      !refHostAllowed(server_.header("Origin"), host)) {
    sendError(403, "forbidden_origin", "Origin host is not this device");
    return false;
  }
  if (server_.hasHeader("Referer") &&
      !refHostAllowed(server_.header("Referer"), host)) {
    sendError(403, "forbidden_origin", "Referer host is not this device");
    return false;
  }
  const String tok = server_.header(kAuthHeaderName);
  if (!adminToken_.length() || tok != adminToken_) {
    sendError(401, "unauthorized", "missing or invalid X-SCRATCHSOR-Token");
    return false;
  }
  return true;
}

bool ScratchsorWebServer::serveStatic(const String& path) {
  if (path.indexOf("..") >= 0) return false;
  String p = path;
  if (p.endsWith("/")) p += "index.html";
  File f = LittleFS.open(p, "r");
  if (!f || f.isDirectory()) return false;
  server_.sendHeader("Cache-Control", "no-store, max-age=0");
  server_.streamFile(f, contentTypeFor(p));
  f.close();
  return true;
}

void ScratchsorWebServer::handleNotFound() {
  if (server_.method() == HTTP_OPTIONS) {  // unmatched-method falls here too
    handleOptions();
    return;
  }
  const String uri = server_.uri();
  // OS captive-portal probes get the portal page — DNS already pointed them
  // here, so serving /index.html directly is the least surprising answer.
  static const char* kProbes[] = {
      "/generate_204", "/gen_204", "/hotspot-detect.html",
      "/ncsi.txt", "/connecttest.txt", "/redirect", "/fwlink"};
  for (const char* p : kProbes) {
    if (uri == p) {
      if (!serveStatic("/index.html")) sendError(404, "not_found", uri.c_str());
      return;
    }
  }
  if (serveStatic(uri)) return;
  sendError(404, "not_found", uri.c_str());
}

void ScratchsorWebServer::handleOptions() {
  // The embedded UI is same-origin and needs no CORS grant; a foreign
  // Origin still gets the explicit 403 so abuse is visible.
  if (server_.hasHeader("Origin") &&
      !refHostAllowed(server_.header("Origin"), server_.hostHeader())) {
    sendError(403, "forbidden_origin", "Origin host is not this device");
    return;
  }
  server_.sendHeader("Allow", "GET, POST, OPTIONS");
  server_.send(204, "text/plain", "");
}

// ---- routes ---------------------------------------------------------------

void ScratchsorWebServer::registerRoutes() {
  server_.on("/api/v1/phone",HTTP_POST,[this]{
    if(!requireAuth())return;
    const String body=server_.arg("plain");
    if(body.length()>1024){sendError(413,"too_large","Request too large");return;}
    JsonDocument request,response;String error;
    if(deserializeJson(request,body)||!request.is<JsonObject>()){sendError(400,"bad_request","JSON object required");return;}
    if(!gPhone.command(request.as<JsonVariantConst>(),response,error)){sendError(409,"phone_state",error.c_str());return;}
    String json;serializeJson(response,json);sendJson(200,json.c_str());
  });
  server_.onNotFound([this] { handleNotFound(); });

  // -- identity / health / sensors --
  server_.on("/api/v1/device", HTTP_GET, [this] {
    DeviceConfig c;
    svc_.storage->loadConfig(c);
    String name = WiFi.softAPSSID();
    if (!name.length()) name = c.deviceName;
    JsonDocument doc;
    doc["name"] = name;
    doc["product"] = kProductName;
    doc["fw"] = kFirmwareVer;
    doc["overlapModel"] = gPhone.modelVersion();
    doc["api"] = kApiVersion;
    doc["uptimeMs"] = millis();
    doc["heap"] = ESP.getFreeHeap();
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  server_.on("/api/v1/health", HTTP_GET, [this] {
    HealthReport h;
    if (svc_.sensors) svc_.sensors->health(h);
    if (!h.uptimeMs) h.uptimeMs = millis();
    if (!h.freeHeap) h.freeHeap = ESP.getFreeHeap();
    JsonDocument doc;
    doc["mux"] = h.muxFound;
    doc["muxAddr"] = h.muxAddr;
    doc["oled"] = svc_.oled ? svc_.oled->present() : h.oledFound;
    doc["oledAddr"] = svc_.oled ? svc_.oled->addr() : h.oledAddr;
    doc["controller"] = oledCtrlStr(svc_.oled ? svc_.oled->controller()
                                            : h.oledController);
    doc["oledScreen"] = svc_.oled ? (unsigned)svc_.oled->activeScreen() : 0;
    doc["oledLastDisplayedMs"] = svc_.oled ? svc_.oled->lastDisplayedMs() : 0;
    doc["oledRefreshPending"] = svc_.oled && svc_.oled->refreshPending();
    doc["rate"] = h.measuredRateHz;
    doc["phoneMeasuring"] = gPhone.measuring();
    doc["phoneCapturePending"] = gPhone.pendingCapture();
    doc["uptimeMs"] = h.uptimeMs;
    doc["heap"] = h.freeHeap;
    JsonArray arr = doc["sensors"].to<JsonArray>();
    for (uint8_t i = 0; i < kSensorCount; ++i) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = i;
      o["st"] = toString(h.sensor[i].status);
      o["ok"] = h.sensor[i].okCount;
      o["err"] = h.sensor[i].errCount;
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  server_.on("/api/v1/sensors", HTTP_GET, [this] {
    SensorFrame f;   // zeros when no frame yet — ids/st=UNKNOWN stay honest
    if (svc_.sensors) svc_.sensors->latest(f);
    JsonDocument doc;
    doc["seq"] = f.seq;
    doc["ts"] = f.timestampMs;
    doc["rate"] = svc_.sensors ? svc_.sensors->measuredRateHz() : 0.0f;
    JsonArray arr = doc["s"].to<JsonArray>();
    for (uint8_t i = 0; i < kSensorCount; ++i) {
      readingToJson(arr.add<JsonObject>(), f.sensors[i], i, /*raw=*/true);
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  // -- config --
  server_.on("/api/v1/config", HTTP_GET, [this] {
    DeviceConfig c;
    svc_.storage->loadConfig(c);
    JsonDocument doc;
    configToJson(doc.to<JsonObject>(), c);
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  server_.on("/api/v1/config", HTTP_POST, [this] {
    if (!requireAuth()) return;
    JsonDocument body;
    if (deserializeJson(body, server_.arg("plain"))) {
      sendError(400, "bad_request", "invalid JSON body");
      return;
    }
    DeviceConfig c;
    svc_.storage->loadConfig(c);
    const OledConfig oledBefore = c.oled;
    const SensorConfig sensorBefore = c.sensor;
    const FilterConfig filterBefore = c.filter;
    const uint32_t signatureBefore=fingerprintSignature(c);
    if(svc_.sensors && svc_.sensors->recording()){sendError(409,"busy","Wait for recording before changing settings");return;}
    if(svc_.calib && svc_.calib->busy()){sendError(409,"busy","Cancel capture before changing settings");return;}
    if(!body.is<JsonObject>()){sendError(400,"bad_request","JSON object required");return;}
    if(!body["reach"].isNull()) {
      auto r=body["reach"];
      if(!r.is<JsonObject>() || (!r["profile"].isNull() && (!r["profile"].is<int>() || r["profile"].as<int>()<0 || r["profile"].as<int>()>3)) ||
         (!r["selected"].isNull() && !r["selected"].is<bool>()) ||
         (!r["setup"].isNull() && !r["setup"].is<const char*>())) {
        sendError(400,"bad_request","Invalid reach profile, selected flag or setup");return;
      }
      c.reach.profile=r["profile"] | c.reach.profile;
      c.reach.selected=r["selected"] | c.reach.selected;
      const char* text=r["setup"];
      if(text){if(strlen(text)>=sizeof c.reach.setup){sendError(400,"bad_request","Setup maximum is 95 UTF-8 bytes");return;}
        for(const char* x=text;*x;++x)if(uint8_t(*x)<32){sendError(400,"bad_request","Setup cannot contain control characters");return;}
        strlcpy(c.reach.setup,text,sizeof c.reach.setup);}
    }

    c.gap.enabled = body["gap"]["enabled"] | c.gap.enabled;
    c.gap.fixedGeometry = body["gap"]["fixedGeometry"] | c.gap.fixedGeometry;
    const int gapSensor = body["gap"]["sensorId"] | (int)c.gap.sensorId;
    const int gapReference = body["gap"]["reference"] | (int)c.gap.reference;
    const char* setup = body["gap"]["setup"];
    if (gapSensor < 0 || gapSensor >= kSensorCount || gapReference < 0 ||
        gapReference > 1 || (setup && strlen(setup) >= sizeof c.gap.setup)) {
      sendError(400, "bad_request", "invalid gap sensor, reference or setup (max 95 bytes)");
      return;
    }
    c.gap.sensorId = (uint8_t)gapSensor;
    c.gap.reference = (uint8_t)gapReference;
    if (setup) strlcpy(c.gap.setup, setup, sizeof c.gap.setup);

    const char* dn = body["deviceName"];
    if (dn && *dn) strlcpy(c.deviceName, dn, sizeof(c.deviceName));
    if (!body["apPersistent"].isNull())
      c.apPersistent = body["apPersistent"].as<bool>();
    // Merge — absent keys keep current values via the | default operator.
    c.sensor.gain             = body["sensor"]["gain"]       | c.sensor.gain;
    c.sensor.resolution       = body["sensor"]["resolution"] | c.sensor.resolution;
    c.sensor.oversampling     = body["sensor"]["osr"]        | c.sensor.oversampling;
    c.sensor.digitalFilter    = body["sensor"]["digFilt"]    | c.sensor.digitalFilter;
    c.sensor.sampleRateHz     = body["sensor"]["rateHz"]     | c.sensor.sampleRateHz;
    c.sensor.readTemperature  = body["sensor"]["temp"]       | c.sensor.readTemperature;
    c.filter.movingAvgEnabled = body["filter"]["avg"]        | c.filter.movingAvgEnabled;
    c.filter.movingAvgWindow  = body["filter"]["avgWin"]     | c.filter.movingAvgWindow;
    c.filter.medianEnabled    = body["filter"]["median"]     | c.filter.medianEnabled;
    c.filter.medianWindow     = body["filter"]["medWin"]     | c.filter.medianWindow;
    c.filter.lowpassEnabled   = body["filter"]["lp"]         | c.filter.lowpassEnabled;
    c.filter.lowpassAlpha     = body["filter"]["alpha"]      | c.filter.lowpassAlpha;
    c.filter.baselineRemoval  = body["filter"]["baseline"]   | c.filter.baselineRemoval;
    c.filter.outlierSigma     = body["filter"]["outlierSigma"] | c.filter.outlierSigma;
    c.oled.refreshHz          = body["oled"]["refreshHz"]    | c.oled.refreshHz;
    c.oled.rotateSec          = body["oled"]["rotateSec"]    | c.oled.rotateSec;
    c.oled.flipDisplay        = body["oled"]["flip"]         | c.oled.flipDisplay;
    c.oled.controllerOverride = body["oled"]["controller"]   | c.oled.controllerOverride;
    // Clamp everything to hardware-safe bounds.
    c.sensor.gain             = clampT<uint8_t>(c.sensor.gain, 0, 7);
    c.sensor.resolution       = clampT<uint8_t>(c.sensor.resolution, 0, 3);
    c.sensor.oversampling     = clampT<uint8_t>(c.sensor.oversampling, 0, 3);
    c.sensor.digitalFilter    = clampT<uint8_t>(c.sensor.digitalFilter, 0, 7);
    c.sensor.sampleRateHz     = clampT(c.sensor.sampleRateHz,
                                      kSampleRateMinHz, kSampleRateMaxHz);
    c.filter.movingAvgWindow  = clampT<uint8_t>(c.filter.movingAvgWindow, 1, 32);
    c.filter.medianWindow     = clampT<uint8_t>(c.filter.medianWindow, 3, 15);
    c.filter.lowpassAlpha     = clampT(c.filter.lowpassAlpha, 0.0f, 1.0f);
    c.filter.outlierSigma     = clampT(c.filter.outlierSigma, 0.0f, 32.0f);
    c.oled.refreshHz          = clampT<uint8_t>(c.oled.refreshHz, 1, 30);
    c.oled.rotateSec          = clampT<uint8_t>(c.oled.rotateSec, 0, 120);
    c.oled.controllerOverride = clampT<uint8_t>(c.oled.controllerOverride, 0, 2);

    const bool signalChanged = memcmp(&c.sensor, &sensorBefore, sizeof c.sensor) != 0 ||
                               memcmp(&c.filter, &filterBefore, sizeof c.filter) != 0;
    if (c.sensor.gain != 7 || c.sensor.resolution != 0 || c.sensor.oversampling != 0 ||
        c.sensor.digitalFilter != 0 || c.sensor.sampleRateHz != 50.f || !c.sensor.readTemperature) {
      sendError(409, "model_settings", "The phone v4 model requires gain 7, resolution 0, OSR 0, digital filter 0, temperature on and 50 Hz"); return;
    }
    if (!c.apPersistent) {
      sendError(409, "phone_wifi", "Keep device Wi-Fi enabled for the phone interface"); return;
    }
    if (signalChanged && gPhone.measuring()) {
      sendError(409, "busy", "Finish the phone measurement before changing signal settings"); return;
    }
    const bool experimentChanged = fingerprintSignature(c) != signatureBefore;
    if (!c.filter.baselineRemoval || !std::isfinite(c.sensor.sampleRateHz) || !std::isfinite(c.filter.lowpassAlpha) || !std::isfinite(c.filter.outlierSigma)) {
      sendError(400, "bad_request", "Fingerprinting requires baseline removal and finite signal settings"); return;
    }
    if (experimentChanged && svc_.calib && svc_.calib->hasData() &&
        !(body["resetCalibration"] | false)) {
      sendError(409, "calibration_exists", "enable clear calibration when changing the fixture or signal settings");
      return;
    }

    // Reset must succeed before changed feature coordinates can be activated.
    if(experimentChanged && svc_.calib && !svc_.calib->clearAll()) {
      sendError(500,"storage_error",svc_.calib->detail());return;
    }
    const bool saved = svc_.storage->saveConfig(c);
    if (!saved) { sendError(500, "storage_error", "configuration not saved"); return; }
    if (svc_.calib) svc_.calib->configure(c);

    if (signalChanged) svc_.sensors->requestConfig(c.sensor, c.filter);
    svc_.wifi->setApPersistent(c.apPersistent);
    if (svc_.oled && memcmp(&c.oled, &oledBefore, sizeof(OledConfig)) != 0) {
      svc_.oled->reconfigure(c.oled);
    }

    JsonDocument doc;
    configToJson(doc.to<JsonObject>(), c);
    doc["persisted"] = saved;
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  // -- wifi --
  server_.on("/api/v1/wifi/status", HTTP_GET, [this] {
    JsonDocument doc;
    wifiStatusToJson(doc.to<JsonObject>(), svc_.wifi->status());
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  server_.on("/api/v1/wifi/scan", HTTP_GET, [this] {
    // Trigger+read: an unarmed call kicks the async scan and reports
    // done:false; polling returns results once, then re-arms for next time.
    if (!sScanArmed) {
      svc_.wifi->startScan();
      sScanArmed = true;
    }
    JsonDocument doc;
    if (!svc_.wifi->scanDone()) {
      doc["done"] = false;
    } else {
      sScanArmed = false;
      String ss[16];
      int rr[16];
      bool op[16];
      const int n = svc_.wifi->scanResults(ss, rr, op, 16);
      doc["done"] = true;
      JsonArray nets = doc["nets"].to<JsonArray>();
      for (int i = 0; i < n; ++i) {
        JsonObject o = nets.add<JsonObject>();
        o["ssid"] = ss[i];
        o["rssi"] = rr[i];
        o["open"] = op[i];
      }
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  server_.on("/api/v1/wifi/connect", HTTP_POST, [this] {
    if (!requireAuth()) return;
    JsonDocument body;
    if (deserializeJson(body, server_.arg("plain"))) {
      sendError(400, "bad_request", "invalid JSON body");
      return;
    }
    const char* ssid = body["ssid"];
    const char* pass = body["pass"] | "";
    if (!ssid || !*ssid) {
      sendError(400, "bad_request", "ssid required");
      return;
    }
    svc_.wifi->connectSta(ssid, pass);
    // Persist immediately: the attempt is bounded (kStaConnectTimeoutMs) and
    // the SoftAP stays up meanwhile, so a wrong saved password cannot strand
    // the device — user can still reach the portal and re-provision.
    if (!svc_.storage->saveWifi(ssid, pass)) {
      SCR_LOGW("saveWifi failed — creds volatile until next save");
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["state"] = toString(WifiStaState::CONNECTING);
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  server_.on("/api/v1/wifi/forget", HTTP_POST, [this] {
    if (!requireAuth()) return;
    svc_.wifi->forgetSta();
    svc_.storage->clearWifi();
    sendJson(200, "{\"ok\":true}");
  });

  // -- fingerprint calibration (same handlers over Wi-Fi and USB) --
  server_.on("/api/v1/calibration", HTTP_GET, [this] {
    if(!svc_.calib){sendError(503,"unavailable","Calibration unavailable");return;}
    const auto& cal=*svc_.calib;JsonDocument doc;
    doc["phase"]=cal.phaseName();doc["detail"]=cal.detail();doc["ready"]=cal.modelValidated();
    doc["baselineReady"]=cal.baselineReady();doc["profileId"]=cal.profile();doc["selected"]=cal.selected();
    doc["setup"]=cal.reachConfig().setup;doc["count"]=cal.pointCount();doc["max"]=kFingerprintCapacity;
    doc["collected"]=cal.collectedThisPoint();doc["target"]=cal.collectTarget();
    calibModelToJson(doc["model"].to<JsonObject>(),cal.model());
    auto profiles=doc["profiles"].to<JsonArray>();
    for(unsigned p=0;p<4;++p){auto o=profiles.add<JsonObject>();o["id"]=p;o["name"]=profileName(p);
      o["ready"]=cal.baselineReady() && cal.dataset().models[p].ready;}
    auto points=doc["points"].to<JsonArray>();
    for(unsigned i=0;i<cal.pointCount();++i){const auto& p=cal.points()[i];auto o=points.add<JsonObject>();
      o["id"]=p.id;o["profileId"]=p.profile;o["refMm"]=p.referenceMm;o["role"]=p.test?"test":"train";
      o["category"]=categoryName(reachCategory(p.profile,p.referenceMm));}
    String out;serializeJson(doc,out);sendJson(200,out.c_str());
  });
  server_.on("/api/v1/calibration/start", HTTP_POST, [this] {
    if(!requireAuth())return;
    if(svc_.sensors && svc_.sensors->recording()){sendError(409,"busy","Wait for recording before calibration");return;}
    if(!svc_.calib){sendError(503,"unavailable","Calibration unavailable");return;}
    JsonDocument body;
    if(deserializeJson(body,server_.arg("plain")) || !body.is<JsonObject>()){
      sendError(400,"bad_request","JSON object required");return;}
    const char* phase=body["phase"] | "";bool ok=false;
    if(!strcmp(phase,"baseline")) {
      if(!body["resetCalibration"].isNull() && !body["resetCalibration"].is<bool>()){
        sendError(400,"bad_request","resetCalibration must be boolean");return;}
      ok=svc_.calib->startBaseline(100,body["resetCalibration"] | false);
    } else if(!strcmp(phase,"collect")) {
      const char* role=body["role"] | "";
      if(!body["refMm"].is<float>() || !referenceAllowed(body["refMm"].as<float>()) ||
         (strcmp(role,"train") && strcmp(role,"test")) || !(body["repositioned"].is<bool>() && body["repositioned"].as<bool>())){
        sendError(400,"bad_request","Signed refMm (-500..500), role train|test and repositioned:true required");return;}
      ok=svc_.calib->startCollect(body["refMm"].as<float>(),!strcmp(role,"test"));
    } else {sendError(400,"bad_request","phase must be baseline|collect");return;}
    if(!ok){sendError(409,"invalid_state",svc_.calib->detail());return;}
    sendJson(200,"{\"ok\":true}");
  });
  server_.on("/api/v1/calibration/fit", HTTP_POST, [this] {
    if(!requireAuth())return;
    if(!svc_.calib){sendError(503,"unavailable","Calibration unavailable");return;}
    if(!svc_.calib->finishAndFit()){sendError(409,"invalid_state",svc_.calib->detail());return;}
    JsonDocument doc;doc["ok"]=true;doc["ready"]=svc_.calib->modelValidated();
    calibModelToJson(doc["model"].to<JsonObject>(),svc_.calib->model());
    String out;serializeJson(doc,out);sendJson(200,out.c_str());
  });
  server_.on("/api/v1/calibration/delete", HTTP_POST, [this] {
    if(!requireAuth())return;
    if(!svc_.calib){sendError(503,"unavailable","Calibration unavailable");return;}
    JsonDocument body;
    if(deserializeJson(body,server_.arg("plain")) || !body["id"].is<uint32_t>() || body["id"].as<uint32_t>()==0){
      sendError(400,"bad_request","Positive trial id required");return;}
    if(!svc_.calib->deletePoint(body["id"].as<uint32_t>())){sendError(409,"invalid_state",svc_.calib->detail());return;}
    sendJson(200,"{\"ok\":true}");
  });
  server_.on("/api/v1/calibration/cancel", HTTP_POST, [this] {
    if(!requireAuth())return;
    if(!svc_.calib || !svc_.calib->cancel()){sendError(409,"invalid_state","No capture running");return;}
    sendJson(200,"{\"ok\":true}");
  });
  server_.on("/api/v1/calibration/reset", HTTP_POST, [this] {
    if(!requireAuth())return;
    if(svc_.sensors && svc_.sensors->recording()){sendError(409,"busy","Wait for recording before resetting calibration");return;}
    if(!svc_.calib || !svc_.calib->clearAll()){sendError(409,"invalid_state","Reset failed; finish/cancel capture first");return;}
    sendJson(200,"{\"ok\":true}");
  });
  server_.on("/api/v1/calibration.csv", HTTP_GET, [this] {
    if(!svc_.calib){sendError(503,"unavailable","Calibration unavailable");return;}
    const auto& cal=*svc_.calib;
    String setup=cal.reachConfig().setup;setup.replace("\"","\"\"");
    if(setup.length() && (setup[0]=='=' || setup[0]=='+' || setup[0]=='-' || setup[0]=='@'))setup="'"+setup;
    server_.setContentLength(CONTENT_LENGTH_UNKNOWN);server_.send(200,"text/csv","");
    server_.sendContent("trial_id,timestamp_ms,profile_id,profile,setup,signature,role,reference_cm,category,samples,s1_x,s1_y,s1_z,s2_x,s2_y,s2_z,s3_x,s3_y,s3_z,s4_x,s4_y,s4_z\n");
    for(unsigned i=0;i<cal.pointCount();++i){const auto& p=cal.points()[i];char start[192];
      snprintf(start,sizeof start,"%lu,%lu,%u,%s,",(unsigned long)p.id,(unsigned long)p.timestampMs,p.profile,profileName(p.profile));
      String row=start;row+="\""+setup+"\",";
      snprintf(start,sizeof start,"%lu,%s,%.9g,%s,%u",(unsigned long)cal.dataset().signature,p.test?"test":"train",
        double(p.referenceMm)/10,categoryName(reachCategory(p.profile,p.referenceMm)),kFeatureFrames);row+=start;
      for(float x:p.features){char feature[32];snprintf(feature,sizeof feature,",%.9g",double(x));row+=feature;}
      row+='\n';server_.sendContent(row);
    }
    server_.sendContent("");
  });

  // -- measurements / recorder --
  server_.on("/api/v1/measurements", HTTP_GET, [this] {
    JsonDocument doc;
    JsonObject est = doc["est"].to<JsonObject>();
    if(svc_.measure){svc_.measure->update();classificationToJson(est,svc_.measure->estimate());}
    else classificationToJson(est,ClassificationEstimate{});
    JsonObject rec = doc["record"].to<JsonObject>();
    rec["active"] = svc_.sensors ? svc_.sensors->recording() : false;
    rec["count"] = svc_.sensors ? svc_.sensors->recordCount() : 0;
    rec["max"] = kRecordMaxFrames;
    rec["durationMs"] = kRecordDurationMs;
    rec["remainingMs"] = svc_.sensors ? svc_.sensors->recordRemainingMs() : 0;
    rec["refMm"] = svc_.sensors ? svc_.sensors->recordReferenceMm() : 0;
    rec["startedMs"] = svc_.sensors ? svc_.sensors->recordStartedMs() : 0;
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  server_.on("/api/v1/record", HTTP_POST, [this] {
    sendError(410, "pc_capture_only", "Record and export through the PC USB session dashboard");
    return;
    if (!requireAuth()) return;
    JsonDocument body;
    const String raw = server_.arg("plain");
    if (deserializeJson(body, raw) || !body.is<JsonObject>() || !body["active"].is<bool>()) {
      sendError(400, "bad_request", "JSON object with boolean active required");
      return;
    }
    const bool active = body["active"] | false;
    if (!svc_.sensors) {
      sendError(500, "internal", "sensor service unavailable");
      return;
    }
    if (active) {
      if (!body["refMm"].is<float>() || !referenceAllowed(body["refMm"].as<float>())) {
        sendError(400, "bad_request", "Measured refMm (-500..500) required");return;
      }
      if (svc_.calib && svc_.calib->busy()) {sendError(409,"busy","Cancel calibration before recording");return;}
      if (!svc_.sensors->recordStart(body["refMm"].as<float>())) {
        sendError(409,"invalid_state","Recorder busy, unavailable, or no fresh healthy sensor readings");return;
      }
    } else svc_.sensors->recordStop();
    const bool rec = svc_.sensors->recording();
    gWs.broadcastRecord(rec, svc_.sensors->recordCount(), kRecordMaxFrames);
    JsonDocument doc;
    doc["ok"] = true;
    doc["active"] = rec;
    doc["count"] = svc_.sensors->recordCount();
    doc["max"] = kRecordMaxFrames;
    doc["durationMs"] = kRecordDurationMs;
    doc["remainingMs"] = svc_.sensors->recordRemainingMs();
    doc["refMm"] = svc_.sensors->recordReferenceMm();
    doc["startedMs"] = svc_.sensors->recordStartedMs();
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
  });

  // -- exports (chunked — never builds the whole dump in RAM) --
  server_.on("/api/v1/export.csv", HTTP_GET, [this] {
    sendError(410, "pc_capture_only", "Record and export through the PC USB session dashboard");
    return;
    if (!svc_.sensors) {
      sendError(500, "internal", "sensor service unavailable");
      return;
    }
    if (svc_.sensors->recording()) {
      sendError(409, "recording_active", "stop recording before downloading");
      return;
    }
    DeviceConfig c;
    svc_.storage->loadConfig(c);
    const uint16_t expected = svc_.sensors->recordCount();
    server_.setContentLength(CONTENT_LENGTH_UNKNOWN);  // chunked
    server_.send(200, "text/csv", "");
    char hdr[384];
    snprintf(hdr,sizeof hdr,"# product=%s fw=%s api=%s\n# rate_hz=%.1f recorded=%u\nseq,ts,id,raw_x,raw_y,raw_z,raw_m,st,ref_mm,elapsed_ms,trial_start_ms,valid,saturated,temp_c,quality\n",
      kProductName,kFirmwareVer,kApiVersion,double(c.sensor.sampleRateHz),unsigned(expected));
    server_.sendContent(hdr);

    // Recording is stopped, so offsets remain stable throughout the export.
    SensorFrame chunk[8];
    uint16_t emitted = 0;
    String buf;
    while (emitted < expected) {
      const uint16_t n = svc_.sensors->recordCopy(chunk, (uint16_t)8, emitted);
      if (!n) break;
      buf = "";
      for (uint16_t k = 0; k < n; ++k) {
        const SensorFrame& f = chunk[k];
        for (uint8_t i = 0; i < kSensorCount; ++i) {
          const SensorReading& r = f.sensors[i];
          char line[384];
          snprintf(line, sizeof(line),
                   "%lu,%lu,%u,%.9g,%.9g,%.9g,%.9g,%s,%.9g,%lu,%lu,%u,%u,%.9g,%.9g\n",
                   (unsigned long)f.seq, (unsigned long)f.timestampMs, i,
                   (double)r.raw.x, (double)r.raw.y, (double)r.raw.z,
                   (double)r.magnitudeRaw, toString(r.status),
                   double(svc_.sensors->recordReferenceMm()),
                   (unsigned long)(f.timestampMs - svc_.sensors->recordStartedMs()),
                   (unsigned long)svc_.sensors->recordStartedMs(),
                   unsigned(r.valid), unsigned(r.saturated), double(r.temperatureC), double(r.quality));
          buf += line;
        }
      }
      emitted += n;
      server_.sendContent(buf);
    }
    server_.sendContent("");  // terminating chunk
  });

  server_.on("/api/v1/export.json", HTTP_GET, [this] {
    sendError(410, "pc_capture_only", "Record and export through the PC USB session dashboard");
    return;
    if (!svc_.sensors) {
      sendError(500, "internal", "sensor service unavailable");
      return;
    }
    if (svc_.sensors->recording()) {
      sendError(409, "recording_active", "stop recording before downloading");
      return;
    }
    DeviceConfig c;
    svc_.storage->loadConfig(c);
    const uint16_t expected = svc_.sensors->recordCount();

    server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server_.send(200, "application/json", "");

    JsonDocument meta;
    JsonObject mo = meta["meta"].to<JsonObject>();
    mo["product"] = kProductName;
    mo["fw"] = kFirmwareVer;
    mo["api"] = kApiVersion;
    mo["uptimeMs"] = millis();
    mo["rateHz"] = c.sensor.sampleRateHz;
    mo["recorded"] = expected;
    mo["refMm"] = svc_.sensors->recordReferenceMm();
    mo["startedMs"] = svc_.sensors->recordStartedMs();
    mo["durationMs"] = kRecordDurationMs;
    configToJson(mo["config"].to<JsonObject>(), c);
    JsonObject cm = mo["calib"].to<JsonObject>();
    if (svc_.calib) calibModelToJson(cm, svc_.calib->model());
    else            cm["type"] = "NONE";
    String out;
    serializeJson(meta, out);
    out.remove(out.length() - 1);  // Keep the root open for streamed frames.
    out += F(",\"frames\":[");
    server_.sendContent(out);

    SensorFrame chunk[8];
    uint16_t emitted = 0;
    bool first = true;
    while (emitted < expected) {
      const uint16_t n = svc_.sensors->recordCopy(chunk, (uint16_t)8, emitted);
      if (!n) break;
      out = "";
      for (uint16_t k = 0; k < n; ++k) {
        JsonDocument fd;
        const SensorFrame& f = chunk[k];
        fd["seq"] = f.seq;
        fd["ts"] = f.timestampMs;
        JsonArray arr = fd["s"].to<JsonArray>();
        for (uint8_t i = 0; i < kSensorCount; ++i) {
          readingToJson(arr.add<JsonObject>(), f.sensors[i], i, /*raw=*/true);
        }
        if (!first) out += ',';
        first = false;
        String frameJson;
        serializeJson(fd, frameJson);
        out += frameJson;
      }
      emitted += n;
      server_.sendContent(out);
    }
    server_.sendContent(F("]}"));
    server_.sendContent("");  // terminating chunk
  });

  // -- maintenance --
  server_.on("/api/v1/restart", HTTP_POST, [this] {
    if (!requireAuth()) return;
    sendJson(200, "{\"ok\":true}");
    SCR_LOGI("restart requested via API");
    sRestartAtMs = millis() + 300;
  });

  server_.on("/api/v1/factory_reset", HTTP_POST, [this] {
    if (!requireAuth()) return;
    bool keepWifi = true;
    const String raw = server_.arg("plain");
    if (raw.length()) {
      JsonDocument body;
      if (deserializeJson(body, raw) || !body.is<JsonObject>() ||
          (body["keepWifi"].is<JsonVariant>() && !body["keepWifi"].is<bool>())) {
        sendError(400, "bad_request", "expected an object with optional boolean keepWifi");
        return;
      }
      keepWifi = body["keepWifi"] | true;
    }
    const bool ok = keepWifi ? svc_.storage->factoryResetKeepWifi()
                             : svc_.storage->factoryResetAll();
    SCR_LOGW("factory_reset keepWifi=%d ok=%d", (int)keepWifi, (int)ok);
    JsonDocument doc;
    doc["ok"] = ok;
    String out;
    serializeJson(doc, out);
    sendJson(200, out.c_str());
    sRestartAtMs = millis() + 300;  // reboot into the reset state
  });
}

}  // namespace scratchsor
