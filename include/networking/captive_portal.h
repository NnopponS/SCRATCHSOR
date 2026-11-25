#pragma once
// Captive-portal DNS: answers every DNS query with the AP address while in
// AP mode so OS connectivity probes land on the portal. A few well-known
// probe URLs are also handled at the HTTP layer (see web_server.cpp).
// Always document the direct fallback http://192.168.4.1 — portal behavior
// varies by OS.

#include <DNSServer.h>
#include <cstdint>
#include "config.h"

namespace scratchsor {

class CaptivePortal {
public:
  // bindIp: address returned for every query (the AP IP).
  bool begin(const IPAddress& bindIp);
  void end();
  void update();           // pump DNS requests; call each loop
  bool running() const { return running_; }

private:
  DNSServer dns_;
  bool running_ = false;
};

extern CaptivePortal gPortal;

}  // namespace scratchsor
