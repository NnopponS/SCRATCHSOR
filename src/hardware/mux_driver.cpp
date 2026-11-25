// MuxDriver — typed wrapper over PCA9548A channel selection plus the
// per-channel device map used by the boot diagnostics.

#include "hardware/mux_driver.h"
#include "hardware/i2c_manager.h"
#include "diagnostics/diagnostics.h"
#include <cstdio>
#include <cstring>

namespace scratchsor {

MuxDriver gMux{gI2c};

uint8_t MuxDriver::detect() {
  addr_ = bus_.detectMux();
  if (!addr_) SCR_LOGW("mux: no PCA9548A on 0x70-0x77");
  return addr_;
}

uint8_t MuxDriver::scanAll(MuxScanResult out[kMuxChannels]) {
  uint8_t total = 0;
  for (uint8_t ch = 0; ch < kMuxChannels; ++ch) {
    MuxScanResult& res = lastScan_[ch];
    res = MuxScanResult{};
    res.channel = ch;
    // A failed channel select just leaves this channel's result empty.
    bus_.withChannel(ch, [this, &res]() -> bool {
      for (uint16_t a = 0x03; a <= 0x77; ++a) {
        if ((uint8_t)a == bus_.muxAddr()) continue;  // mux ACKs on every channel
        if (bus_.probe((uint8_t)a) && res.count < 8) {
          res.addresses[res.count++] = (uint8_t)a;
        }
      }
      return true;
    });
    total += res.count;
  }
  scanned_ = true;
  if (out) memcpy(out, lastScan_, sizeof(lastScan_));
  return total;
}

void MuxDriver::formatChannelMap(char* buf, size_t len) {
  if (!buf || len == 0) return;
  // Worst case: " CH7:" + 8 x "0xXX," ~= 46 chars per channel.
  char tmp[kMuxChannels * 48];
  size_t p = 0;
  tmp[0] = '\0';
  for (uint8_t ch = 0; ch < kMuxChannels && p < sizeof(tmp) - 1; ++ch) {
    const MuxScanResult& r = lastScan_[ch];
    p += snprintf(tmp + p, sizeof(tmp) - p, "%sCH%u:", ch ? " " : "",
                  (unsigned)ch);
    if (p >= sizeof(tmp) - 1) break;
    if (r.count == 0) {
      p += snprintf(tmp + p, sizeof(tmp) - p, "-");
    } else {
      for (uint8_t i = 0; i < r.count && p < sizeof(tmp) - 1; ++i) {
        p += snprintf(tmp + p, sizeof(tmp) - p, "%s0x%02X", i ? "," : "",
                      r.addresses[i]);
      }
    }
  }
  tmp[sizeof(tmp) - 1] = '\0';
  snprintf(buf, len, "%s", tmp);
}

}  // namespace scratchsor
