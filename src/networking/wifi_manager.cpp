// Wi-Fi manager implementation: SoftAP + STA state machine, never blocking.
// Credentials persist via storage_service (NVS), so WiFi.persistent(false)
// keeps the SDK from writing its own flash copies. The stored STA password
// is never logged or returned in any status.
//
// Retry policy: a failed/timed-out attempt reschedules startSta() after
// kStaRetryBackoffMs — bounded work per update(), retries forever.

#include <Arduino.h>
// arduino-esp32 (esp32-hal-gpio.h) defines bare DISABLED/ENABLED macros that
// collide with the DISABLED enum members in the frozen device_types.h —
// undef before any project header is parsed. No code here uses the macros.
#undef DISABLED
#undef ENABLED
#include "networking/wifi_manager.h"
#include "diagnostics/diagnostics.h"
#include <cstdio>

namespace scratchsor {

WifiManager gWifi;

namespace {

// Client count seen by the last update() pass — for change detection only;
// status() itself is always recomputed on demand.
uint8_t sLastApClients = 0;

void fireStatus(WifiManager& wm) {
  if (wm.onStatusChange) wm.onStatusChange(wm.status());
}

// Short stable reason strings — staError_ lands verbatim in
// /api/v1/wifi/status and the WS "status" frame.
const char* staReasonStr(uint8_t reason, char* buf, size_t len) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:              return "no_ap";
    case WIFI_REASON_AUTH_FAIL:                return "auth_fail";
    case WIFI_REASON_ASSOC_FAIL:               return "assoc_fail";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:        return "handshake_timeout";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:   return "handshake_timeout";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "key_timeout";
    case WIFI_REASON_BEACON_TIMEOUT:           return "beacon_timeout";
    case WIFI_REASON_AUTH_EXPIRE:              return "auth_expire";
    case WIFI_REASON_ASSOC_EXPIRE:             return "assoc_expire";
    case WIFI_REASON_ASSOC_LEAVE:              return "ap_disconnect";
    case WIFI_REASON_MIC_FAILURE:              return "mic_failure";
    case WIFI_REASON_CONNECTION_FAIL:          return "conn_fail";
    case WIFI_REASON_802_1X_AUTH_FAILED:       return "8021x_fail";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:    return "cipher_rejected";
    default:
      snprintf(buf, len, "reason_%u", reason);
      return buf;
  }
}

// Reasons that are terminal mid-attempt: fail fast instead of waiting out
// the connect timeout. Transient events (ASSOC_LEAVE during handshake etc.)
// keep CONNECTING — the bounded timeout still applies.
bool staReasonIsTerminal(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
      return true;
    default:
      return false;
  }
}

// "SCRATCHSOR-" + last 4 hex digits of the efuse MAC.
String apSsidFromMac() {
  return WifiManager::defaultApSsid();
}

}  // namespace

String WifiManager::defaultApSsid() {
  const uint64_t mac = ESP.getEfuseMac();
  char name[32];
  // ESP.getEfuseMac() stores MAC bytes in little-endian order.
  snprintf(name, sizeof(name), "%s-%02X%02X", kProductName,
           (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 40) & 0xFF));
  return String(name);
}

void WifiManager::begin(const DeviceConfig& cfg, const String& apPassword,
                        const String& staSsid, const String& staPass) {
  cfg_ = cfg;
  apPassword_ = apPassword;
  pendingSsid_ = staSsid;
  pendingPass_ = staPass;
  staState_ = WifiStaState::IDLE;
  staError_ = "";
  staAttemptStartMs_ = 0;
  nextStaRetryMs_ = 0;
  scanInProgress_ = false;
  sLastApClients = 0;

  WiFi.persistent(false);       // NVS (storage_service) owns credentials
  WiFi.setAutoReconnect(false); // bounded retries are driven from update()

  const bool wantSta = pendingSsid_.length() > 0;
  if (!cfg_.apPersistent && wantSta) {
    WiFi.mode(WIFI_STA);                       // STA-only as configured
  } else if (cfg_.apPersistent) {
    WiFi.mode(WIFI_AP_STA);                    // management AP always up
  } else {
    // No credentials AND AP disabled: raise the SoftAP anyway — a dark
    // device can never be re-provisioned (never-stranded fallback).
    SCR_LOGW("no STA creds and apPersistent=false — keeping SoftAP up");
    WiFi.mode(WIFI_AP);
  }

  if (WiFi.getMode() & WIFI_MODE_AP) startAp();

  WiFi.onEvent([this](arduino_event_id_t ev, arduino_event_info_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      this->onStaDisconnected(info.wifi_sta_disconnected.reason);
    }
  });

  if (wantSta) startSta();
}

void WifiManager::startAp() {
  if (!(WiFi.getMode() & WIFI_MODE_AP)) {
    WiFi.mode((wifi_mode_t)(WiFi.getMode() | WIFI_MODE_AP));
  }
  if (apPassword_.length() < 8) {
    // WPA2 minimum — refuse to broadcast an open management network.
    SCR_LOGE("AP password too short (%u chars) — SoftAP not started",
             apPassword_.length());
    return;
  }
  const String ssid = apSsidFromMac();
  if (WiFi.softAP(ssid.c_str(), apPassword_.c_str(), kApChannel,
                  false /*hidden*/, kApMaxClients)) {
    SCR_LOGI("SoftAP up: %s  ip %s", ssid.c_str(),
             WiFi.softAPIP().toString().c_str());
  } else {
    SCR_LOGE("WiFi.softAP() failed for %s", ssid.c_str());
  }
}

void WifiManager::startSta() {
  if (!pendingSsid_.length()) return;
  if (!(WiFi.getMode() & WIFI_MODE_STA)) {
    WiFi.mode((wifi_mode_t)(WiFi.getMode() | WIFI_MODE_STA));
  }
  WiFi.disconnect();  // drop a stale session; STA iface stays up
  WiFi.begin(pendingSsid_.c_str(),
             pendingPass_.length() ? pendingPass_.c_str() : nullptr);
  staAttemptStartMs_ = millis();
  staError_ = "";
  staState_ = WifiStaState::CONNECTING;
  SCR_LOGI("STA connecting to %s", pendingSsid_.c_str());
  fireStatus(*this);
}

void WifiManager::update() {
  const uint32_t now = millis();

  // Kick a queued connect (connectSta sets IDLE) or a FAILED retry whose
  // backoff has elapsed. Never blocks — startSta() itself is non-blocking.
  if (pendingSsid_.length() &&
      staState_ != WifiStaState::CONNECTING &&
      staState_ != WifiStaState::CONNECTED &&
      (int32_t)(now - nextStaRetryMs_) >= 0) {
    startSta();
  }

  if (staState_ == WifiStaState::CONNECTING) {
    const wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      staState_ = WifiStaState::CONNECTED;
      staError_ = "";
      SCR_LOGI("STA connected: %s  ip %s  rssi %d", pendingSsid_.c_str(),
               WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      fireStatus(*this);
    } else if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) {
      staState_ = WifiStaState::FAILED;
      staError_ = (s == WL_NO_SSID_AVAIL) ? "no_ap" : "connect_failed";
      nextStaRetryMs_ = now + kStaRetryBackoffMs;
      SCR_LOGW("STA connect failed: %s", staError_.c_str());
      fireStatus(*this);
    } else if (now - staAttemptStartMs_ > kStaConnectTimeoutMs) {
      staState_ = WifiStaState::FAILED;
      staError_ = "timeout";
      nextStaRetryMs_ = now + kStaRetryBackoffMs;
      WiFi.disconnect();  // abort the stuck attempt; retry on backoff
      SCR_LOGW("STA connect timeout (>%u ms)", kStaConnectTimeoutMs);
      fireStatus(*this);
    }
  }

  // AP client-count change also warrants a status push.
  const uint8_t clients =
      (WiFi.getMode() & WIFI_MODE_AP) ? WiFi.softAPgetStationNum() : 0;
  if (clients != sLastApClients) {
    sLastApClients = clients;
    fireStatus(*this);
  }
}

WifiStatus WifiManager::status() const {
  WifiStatus s;
  const wifi_mode_t m = WiFi.getMode();
  s.mode = (m == WIFI_MODE_APSTA)   ? WifiMode::AP_STA
         : (m == WIFI_MODE_STA)     ? WifiMode::STA_ONLY
                                    : WifiMode::AP_ONLY;
  s.staState = staState_;
  if (m & WIFI_MODE_AP) {
    s.apSsid = WiFi.softAPSSID();
    s.apIp = WiFi.softAPIP();
    s.apClients = WiFi.softAPgetStationNum();
  }
  s.staSsid = pendingSsid_;
  if (staState_ == WifiStaState::CONNECTED) {
    s.staIp = WiFi.localIP();
    s.staRssi = WiFi.RSSI();
  }
  s.staError = staError_;
  return s;
}

void WifiManager::startScan() {
  // Scanning needs the STA interface even while the SoftAP is up.
  if (!(WiFi.getMode() & WIFI_MODE_STA)) {
    WiFi.mode((wifi_mode_t)(WiFi.getMode() | WIFI_MODE_STA));
  }
  WiFi.scanDelete();  // free results of any previous scan
  const int16_t r = WiFi.scanNetworks(true /*async*/);
  scanInProgress_ = (r == WIFI_SCAN_RUNNING);
  if (r == WIFI_SCAN_FAILED) SCR_LOGW("WiFi scan failed to start");
}

bool WifiManager::scanDone() const {
  if (!scanInProgress_) return true;
  // Do not clear scanInProgress_ here (const) — scanResults() owns cleanup.
  return WiFi.scanComplete() != WIFI_SCAN_RUNNING;
}

int WifiManager::scanResults(String* ssids, int* rssis, bool* open, int max) {
  const int n = WiFi.scanComplete();
  scanInProgress_ = false;
  if (n <= 0 || !ssids || !rssis || !open || max <= 0) {
    if (n > 0) WiFi.scanDelete();
    return 0;
  }
  // Repeated max-pick — O(n*max) on a small list, sorted by RSSI desc.
  const int count = n < 24 ? n : 24;  // bound index bookkeeping
  const int bound = max < 16 ? max : 16;
  uint8_t used[24] = {};
  int emitted = 0;
  while (emitted < bound) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
      if (!used[i] && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
    }
    if (best < 0) break;
    used[best] = 1;
    ssids[emitted] = WiFi.SSID(best);
    rssis[emitted] = (int)WiFi.RSSI(best);
    open[emitted] = (WiFi.encryptionType(best) == WIFI_AUTH_OPEN);
    ++emitted;
  }
  WiFi.scanDelete();
  return emitted;
}

void WifiManager::connectSta(const String& ssid, const String& pass) {
  if (!ssid.length()) return;
  pendingSsid_ = ssid;
  pendingPass_ = pass;
  staState_ = WifiStaState::IDLE;  // update() kicks startSta() next pass
  nextStaRetryMs_ = 0;
  staError_ = "";
  fireStatus(*this);
}

void WifiManager::forgetSta() {
  pendingSsid_ = "";
  pendingPass_ = "";
  staState_ = WifiStaState::IDLE;
  staError_ = "";
  WiFi.disconnect(true);  // drops the STA iface; caller clears NVS
  if (!(WiFi.getMode() & WIFI_MODE_AP)) {
    // STA-only box that just forgot its only network — keep it reachable.
    SCR_LOGW("STA forgotten with no AP — raising SoftAP fallback");
    startAp();
  }
  fireStatus(*this);
}

void WifiManager::setApPersistent(bool persistent) {
  if (persistent == cfg_.apPersistent) return;
  cfg_.apPersistent = persistent;
  const wifi_mode_t m = WiFi.getMode();
  if (persistent) {
    if (!(m & WIFI_MODE_AP)) startAp();
  } else if (m & WIFI_MODE_STA) {
    // STA is live — drop just the AP. If the link bounces, the disconnect
    // event drives the normal retry path.
    WiFi.mode(WIFI_STA);
    SCR_LOGI("SoftAP dropped (STA persists)");
  }
  // else: STA not running — keep the SoftAP so the box stays reachable;
  // cfg_ still records the user intent for the next boot.
}

void WifiManager::onStaDisconnected(uint8_t reason) {
  char rb[16];
  const char* r = staReasonStr(reason, rb, sizeof(rb));
  if (staState_ == WifiStaState::CONNECTED) {
    staState_ = WifiStaState::FAILED;
    staError_ = r;
    nextStaRetryMs_ = millis() + kStaRetryBackoffMs;
    SCR_LOGW("STA link lost: %s — retry in %u ms", r, kStaRetryBackoffMs);
    fireStatus(*this);
  } else if (staState_ == WifiStaState::CONNECTING) {
    staError_ = r;  // last seen reason, useful while the attempt continues
    if (staReasonIsTerminal(reason)) {
      staState_ = WifiStaState::FAILED;
      nextStaRetryMs_ = millis() + kStaRetryBackoffMs;
      SCR_LOGW("STA connect failed fast: %s", r);
      fireStatus(*this);
    }
  }
  // IDLE/DISABLED: event is a side effect of our own disconnect — ignore.
}

}  // namespace scratchsor
