#pragma once
// WebSocket server (links2004/WebSockets, port from config kWsPort... the
// WS endpoint is served by the same HTTP server object on path /ws via
// WebSocketsServerCore? No — links2004 WebSocketsServer runs its own port.
// We run it on port 81 AND register a note in the portal page; the frontend
// connects to ws://<host>:81/ws. If we later move it onto :80 this header
// stays stable.)
//
// Broadcasting is non-blocking: a frame is serialized once into a static
// buffer and broadcastTXT() is handed the same buffer; slow/disconnected
// clients are dropped by the library's own ping/pong + disconnect events.
// sendX() methods are safe to call from the app loop only (the acquisition
// task publishes via the sensor service queue, not directly).

#include <Arduino.h>
#include <WebSocketsServer.h>
#include <functional>
#include "config.h"
#include "device_types.h"

namespace scratchsor {

class SensorService;
class WifiManager;
class MeasurementService;

class WsServer {
public:
  WsServer() = default;

  void begin();
  void update();   // ws.loop() + heartbeat scheduling

  uint8_t clientCount() const { return clients_; }

  // Frame producers — serialize into shared buffer, broadcast if clients>0.
  void sendHello();                               // on new client connect
  void broadcastSensors(const SensorFrame& f,
                        const ClassificationEstimate& e, float rateHz);
  void broadcastStatus(const WifiStatus& w, const HealthReport& h);
  void broadcastCalib(const char* phase, float refMm, uint16_t collected,
                      uint16_t target, const char* detail);
  void broadcastRecord(bool active, uint16_t count, uint16_t max);
  void broadcastError(const char* code, const char* detail);

  // Client→server command hook (record toggle, ping, subscribe).
  std::function<void(uint8_t client, const char* json)> onCommand;

  // Returns true when a WS payload carries a valid admin token.
  bool authorized(const char* json) const;
  void setAdminToken(const String& tok) { adminToken_ = tok; }

private:
  void onEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len);

  WebSocketsServer ws_{kWsPort};
  uint8_t clients_ = 0;
  uint32_t lastStatusMs_ = 0;
  uint32_t lastSensorWsMs_ = 0;
  String adminToken_;
  // Cached pointers for periodic status push (set in begin()).
  const WifiStatus* wifi_ = nullptr;
  const HealthReport* health_ = nullptr;
};

extern WsServer gWs;

}  // namespace scratchsor
