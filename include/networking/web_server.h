#pragma once
// HTTP server: LittleFS static files + REST API + captive-probe handling.
// Uses the blocking WebServer from the ESP32 Arduino core; handlers are kept
// short and never wait on sensor hardware. Mutating endpoints enforce the
// admin token + Origin checks (see protocol.h).

#include <Arduino.h>
#include "networking/dashboard_transport.h"
#include "config.h"
#include "device_types.h"

namespace scratchsor {

// Implemented by web_server.cpp — routes are registered against live
// service references supplied at begin().
class SensorService;
class WifiManager;
class CalibrationService;
class MeasurementService;
class StorageService;
class OledManager;
struct Services {
  SensorService* sensors;
  WifiManager* wifi;
  CalibrationService* calib;
  MeasurementService* measure;
  StorageService* storage;
  OledManager* oled;
};

class ScratchsorWebServer {
public:
  void begin(const Services& services, const String& adminToken);
  void update();   // server.handleClient()

  // Re-set token if AP password is regenerated.
  void setAdminToken(const String& t) { adminToken_ = t; }

private:
  void registerRoutes();
  bool requireAuth();            // token + origin check; sends 401/403
  void sendJson(int code, const char* json);
  void sendError(int code, const char* err, const char* detail);
  bool serveStatic(const String& path);   // LittleFS, returns false on miss
  void handleNotFound();                   // captive probes → portal else 404
  void handleOptions();                    // CORS preflight (same-origin only)

  DashboardTransport server_{kHttpPort};
  Services svc_{};
  String adminToken_;
};

extern ScratchsorWebServer gHttp;

}  // namespace scratchsor
