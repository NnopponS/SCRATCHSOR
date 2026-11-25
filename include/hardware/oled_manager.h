#pragma once
// OLED manager for the 0.96" 128x64 module on kMuxChOled.
// Uses U8G2 so both SSD1306 and SH1106 controllers are supported; the
// concrete display object is chosen at runtime from detection + config
// override. I2C controller detection over I2C is inherently limited — see
// detect(): we resolve the address definitively and pick the controller from
// config override, else SSD1306 (documented assumption, overridable at
// runtime in Settings and by /api/v1/config).
//
// All drawing funnels through the shared I2C bus guard on kMuxChOled, so it
// is safe to call show()/render() from the app loop while the acquisition
// task owns the bus most of the time. Refresh is rate-limited; screens
// auto-rotate every cfg.rotateSec unless a screen is pinned.

#include <cstdint>
#include <memory>
#include "config.h"
#include "device_types.h"

class U8G2;

namespace scratchsor {

class I2CManager;

class OledManager {
public:
  // Out-of-line ctor/dtor: unique_ptr<U8G2> needs the complete U8G2 type at
  // the point of instantiation (ctor EH cleanup + dtor), so both live in the
  // .cpp where U8g2lib.h is included.
  explicit OledManager(I2CManager& bus);
  ~OledManager();

  // Resolve OLED address on kMuxChOled (0x3C then 0x3D) and construct the
  // driver. Returns false if no display ACKs — caller must continue headless.
  bool begin(const OledConfig& cfg);

  bool present() const { return (bool)disp_; }
  uint8_t addr() const { return addr_; }
  OledController controller() const { return controller_; }

  // Call on every loop, including at 50 Hz acquisition. Runtime transfers
  // send two tiles at a time and never wait to take the sensor bus.
  void update(bool highRateStreaming = false);
  void useIncrementalRefresh() { incremental_ = true; }
  bool refreshPending() const { return refreshPending_ || redrawRequested_; }
  uint32_t lastDisplayedMs() const { return lastDisplayedMs_; }

  // Push new state for the data-driven screens. Cheap to call every frame;
  // the OLED only redraws at refreshHz.
  void setWifiStatus(const WifiStatus& s) { wifi_ = s; }
  void setSensorFrame(const SensorFrame& f) { frame_ = f; }
  void setEstimate(const ClassificationEstimate& e) { est_ = e; }
  void setGapConfig(const GapConfig& g) { gap_ = g; }
  void setHealth(const HealthReport& h) { health_ = h; }
  void setApPassword(const String& pw) { apPassword_ = pw; }
  void setCalibHint(const String& s) { calibHint_ = s; }

  // Screen control.
  void show(OledScreen s);                    // pin a screen now
  void unpin() { pinned_ = false; }
  OledScreen activeScreen() const { return screen_; }
  void setReadingSensor(uint8_t idx) { readingSensor_ = idx < kSensorCount ? idx : 0; }

  // Re-init controller type at runtime (settings change). Re-probes address.
  bool reconfigure(const OledConfig& cfg);

private:
  bool render();                  // prepare buffer; synchronous send at boot
  void flushChunk();              // short, mutex-guarded runtime transfer
  void renderBoot();
  void renderApInfo();
  void renderApSetup();           // shows AP password — temporary screen
  void renderStaStatus();
  void renderSensorHealth();
  void renderReadings();
  void renderMeasurement();
  void renderLiveSummary();
  OledController pickController(const OledConfig& cfg) const;

  I2CManager& bus_;
  std::unique_ptr<U8G2> disp_;
  uint8_t addr_ = 0;
  OledController controller_ = OledController::UNKNOWN;
  OledConfig cfg_{};
  OledScreen screen_ = OledScreen::BOOT;
  bool pinned_ = false;
  uint32_t lastRenderMs_ = 0;
  uint32_t lastRotateMs_ = 0;
  uint32_t lastDisplayedMs_ = 0;
  uint32_t lastChunkMs_ = 0;
  bool incremental_ = false;
  bool redrawRequested_ = true;
  bool refreshPending_ = false;
  uint8_t nextTile_ = 0;
  uint8_t readingSensor_ = 0;

  WifiStatus wifi_{};
  SensorFrame frame_{};
  ClassificationEstimate est_{};
  GapConfig gap_{};
  HealthReport health_{};
  String apPassword_;
  String calibHint_;
};

extern OledManager gOled;

}  // namespace scratchsor
