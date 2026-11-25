// Captive-portal DNS: while the SoftAP is up, every DNS query is answered
// with the AP IP so OS connectivity probes land on the portal page (the
// HTTP layer answers the well-known probe URLs — see web_server.cpp).
// Portal detection still varies by OS; the documented fallback is the
// direct URL http://192.168.4.1.

#include <Arduino.h>
// See wifi_manager.cpp — DISABLED/ENABLED macros vs device_types.h enums.
#undef DISABLED
#undef ENABLED
#include "networking/captive_portal.h"
#include "diagnostics/diagnostics.h"

namespace scratchsor {

CaptivePortal gPortal;

bool CaptivePortal::begin(const IPAddress& bindIp) {
  if (running_) end();
  running_ = dns_.start(kDnsPort, "*", bindIp);
  if (running_) {
    SCR_LOGI("captive DNS up on %s:%u", bindIp.toString().c_str(), kDnsPort);
  } else {
    SCR_LOGE("captive DNS failed to bind udp/%u", kDnsPort);
  }
  return running_;
}

void CaptivePortal::end() {
  if (!running_) return;
  dns_.stop();
  running_ = false;
}

void CaptivePortal::update() {
  if (running_) dns_.processNextRequest();
}

}  // namespace scratchsor
