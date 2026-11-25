#pragma once
// Thin typed wrapper over PCA9548A/TCA9548A channel selection.
// I2CManager::selectChannel/deselectAll do the real work; this driver exists
// so higher layers have a named, testable object and so channel bookkeeping
// (per-channel device map, scan results) has a home.

#include <cstdint>
#include <cstddef>
#include "config.h"

namespace scratchsor {

class I2CManager;

struct MuxScanResult {
  uint8_t channel = 0;
  uint8_t addresses[8] = {0};   // responding 7-bit addrs on this channel
  uint8_t count = 0;
};

class MuxDriver {
public:
  explicit MuxDriver(I2CManager& bus) : bus_(bus) {}

  // Scan 0x70..0x77 on the primary bus (no channel selected). Returns the
  // mux address found, or 0. Fills addr(). Idempotent.
  uint8_t detect();

  bool present() const { return addr_ != 0; }
  uint8_t addr() const { return addr_; }

  // Scan all 8 channels for ACKing devices (diagnostic/boot use only —
  // do NOT call during normal acquisition). Returns per-channel results.
  // Skips addresses below 0x03/above 0x77 and the mux itself.
  uint8_t scanAll(MuxScanResult out[kMuxChannels]);

  // Human-readable boot banner line, e.g. "CH0: 0x0C".
  void formatChannelMap(char* buf, size_t len);

private:
  I2CManager& bus_;
  uint8_t addr_ = 0;
  MuxScanResult lastScan_[kMuxChannels] = {};
  bool scanned_ = false;
};

extern MuxDriver gMux;

}  // namespace scratchsor
