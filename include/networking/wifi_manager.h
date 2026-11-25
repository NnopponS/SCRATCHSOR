#pragma once
// Wi-Fi manager: SoftAP + STA + AP_STA, persistent credentials, bounded
// timeouts, auto-reconnect, never-stranded fallback.
//
// Security contract:
//  - This bench build uses the AP SSID as its password, per user request.
//  - The same string doubles as the admin token for mutating REST calls.
//  - Stored STA password is never returned by any API or printed to serial.

#include <Arduino.h>
#include <WiFi.h>
#include <functional>
#include "config.h"
#include "device_types.h"

namespace scratchsor {

class StorageService;

class WifiManager {
public:
  WifiManager() = default;
  // Stable name available before the Wi-Fi interfaces are initialized.
  static String defaultApSsid();

  // Bring up SoftAP always (unless STA-only requested) and start STA if
  // credentials exist. Non-blocking: state machine advances in update().
  void begin(const DeviceConfig& cfg, const String& apPassword,
             const String& staSsid, const String& staPass);

  // Advance state machine: reconnect backoff, AP client count, RSSI.
  void update();

  WifiStatus status() const;

  // Provisioning API (called by web_server on validated requests).
  void startScan();                       // async WiFi scan
  bool scanDone() const;                  // scan finished (or failed)
  int  scanResults(String* ssids, int* rssis, bool* open, int max); // returns count
  void connectSta(const String& ssid, const String& pass); // bounded attempt
  void forgetSta();                       // disconnect + caller clears NVS
  void setApPersistent(bool persistent);  // drop/restore SoftAP (reconnect-safe)

  // STA events → reason strings for staError.
  void onStaDisconnected(uint8_t reason);

  String apPassword() const { return apPassword_; }

  // Callback fired when STA connectivity state changes (for WS status push).
  std::function<void(const WifiStatus&)> onStatusChange;

private:
  void startAp();
  void startSta();

  DeviceConfig cfg_{};
  String apPassword_;
  String pendingSsid_, pendingPass_;
  WifiStaState staState_ = WifiStaState::IDLE;
  String staError_;
  uint32_t staAttemptStartMs_ = 0;
  uint32_t nextStaRetryMs_ = 0;
  bool scanInProgress_ = false;
};

extern WifiManager gWifi;

}  // namespace scratchsor
