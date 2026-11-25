// SCRATCHSOR — ESP32 wireless magnetic sensing system.
// Boot sequence (spec §12):
//   serial -> config -> I2C -> mux -> OLED(CH0) -> sensors(CH2-5) -> health
//   -> AP -> DNS+HTTP -> STA reconnect -> acquisition -> WS -> OLED status.
// No stage blocks indefinitely on a failed sibling: networking runs without
// sensors, acquisition runs without Wi-Fi, headless runs without OLED.

#include <Arduino.h>
#include <LittleFS.h>

#include "config.h"
#include "pins.h"
#include "device_types.h"
#include "protocol.h"

#include "hardware/i2c_manager.h"
#include "hardware/mux_driver.h"
#include "hardware/oled_manager.h"
#include "networking/wifi_manager.h"
#include "networking/captive_portal.h"
#include "networking/web_server.h"
#include "networking/websocket_server.h"
#include "services/storage_service.h"
#include "services/sensor_service.h"
#include "services/calibration_service.h"
#include "services/measurement_service.h"
#include "diagnostics/diagnostics.h"
#include "product/phone_product.h"

using namespace scratchsor;

// Global service objects (globals defined in their .cpp files).
static StorageService* storage = &gStorage;
static WifiManager* wifi = &gWifi;
static CaptivePortal* portal = &gPortal;
static ScratchsorWebServer* http = &gHttp;
static WsServer* ws = &gWs;
static OledManager* oled = &gOled;
static MuxDriver* mux = &gMux;
static I2CManager* i2c = &gI2c;

void setup() {
  // Hold a full USB request while the OLED occupies the main loop.
  Serial.setRxBufferSize(8192);
  Serial.begin(kSerialBaud);
  delay(50);
  Serial.println();
  SCR_LOGI("%s firmware %s", kProductName, kFirmwareVer);

  // 2. Load persisted configuration (defaults on first boot / corrupt NVS).
  storage->begin();
  DeviceConfig cfg;
  storage->loadConfig(cfg);
  cfg.apPersistent = true; // phone setup must remain reachable without a router
  cfg.sensor = SensorConfig{}; // v4 was trained with this 50 Hz acquisition setup
  String staSsid, staPass;
  storage->loadWifi(staSsid, staPass);
  // Requested bench access: Wi-Fi password and admin token equal the SSID.
  String apPassword = WifiManager::defaultApSsid();

  // LittleFS for the Web UI assets.
  if (!LittleFS.begin(false)) {
    SCR_LOGE("LittleFS mount failed — Web UI unavailable");
  } else {
    File index = LittleFS.open("/index.html", "r");
    if (index && index.size() > 0) {
      SCR_LOGI("Web UI index ready: %u bytes", (unsigned)index.size());
    } else {
      SCR_LOGE("Web UI index missing — upload the LittleFS image");
    }
  }

  // 3-4. I2C + mux.
  i2c->begin();
  uint8_t muxAddr = mux->detect();
  if (muxAddr) {
    SCR_LOGI("PCA9548A found at 0x%02X", muxAddr);
    char mapbuf[128];
    mux->scanAll(nullptr) /* fills lastScan_ */;
    mux->formatChannelMap(mapbuf, sizeof(mapbuf));
    SCR_LOGI("mux map: %s", mapbuf);
  } else {
    SCR_LOGE("PCA9548A not found — sensor/OLED channels unreachable");
  }

  // 5. OLED on the configured mux channel (headless-safe).
  if (oled->begin(cfg.oled)) {
    oled->setGapConfig(cfg.gap);
    SCR_LOGI("OLED at 0x%02X controller=%d", oled->addr(), (int)oled->controller());
    oled->show(OledScreen::BOOT);
    oled->setApPassword(apPassword);
  } else {
    SCR_LOGW("No OLED on CH%u — continuing headless", (unsigned)kMuxChOled);
  }

  // 6-7. Sensors + health snapshot.
  SensorService* sensors = new SensorService(*i2c);
  gpSensors = sensors;
  uint8_t found = sensors->begin(cfg.sensor, cfg.filter);
  SCR_LOGI("Magnetometers online: %u/4", found);

  HealthReport health;
  buildHealthReport(health, *mux, *sensors, *oled);
  printBootReport(health);

  // 8-10. Wi-Fi: AP up (unique password on OLED), then STA reconnect attempt.
  wifi->begin(cfg, apPassword, staSsid, staPass);
  portal->begin(WiFi.softAPIP());

  Services services{sensors, wifi, nullptr, nullptr, storage, oled};

  // Leave the boot screen before acquisition can contend for the shared bus.
  oled->setWifiStatus(wifi->status());
  oled->setHealth(health);
  oled->setApPassword(apPassword);
  if (oled->present()) oled->show(OledScreen::AP_SETUP);
  oled->useIncrementalRefresh();

  // 11. Acquisition task starts before HTTP so first clients see live data.

  // v4 inference and a transient one-second capture run here; records live on the phone.
  gPhone.begin();
  sensors->start();

  // 12-13. WS + HTTP + OLED status.
  ws->begin();
  ws->setAdminToken(apPassword);
  ws->onCommand = nullptr;  // wired in web_server via REST path instead
  http->begin(services, apPassword);

  SCR_LOGI("AP SSID %s  portal http://%s", WiFi.softAPSSID().c_str(),
           WiFi.softAPIP().toString().c_str());
}

void loop() {
  gWifi.update();
  gPortal.update();
  gHttp.update();
  gWs.update();
  gPhone.update();
  gOled.setWifiStatus(gWifi.status());
  // Live data fan-out: push newest frame to OLED + WS when fresh.
  static uint32_t lastSeq = 0, lastWsMs = 0;
  static bool reportedValidFrame = false;
  static uint32_t lastHealthMs = 0;
  SensorFrame f;
  if (gpSensors->latest(f) && f.seq != lastSeq) {
    lastSeq = f.seq;
    if (!reportedValidFrame && f.validCount > 0) {
      SCR_LOGI("First acquisition frame: %u/%u valid sensors (seq %lu)",
               (unsigned)f.validCount, (unsigned)kSensorCount,
               (unsigned long)f.seq);
      reportedValidFrame = true;
    }
    gOled.setSensorFrame(f);

    if (millis() - lastHealthMs >= 30000) {
      lastHealthMs = millis();
      HealthReport h;
      gpSensors->health(h);
      uint32_t ok = 0, err = 0;
      for (const auto& s : h.sensor) {
        if (s.i2cAddr) { ok += s.okCount; err += s.errCount; }
      }
      SCR_LOGI("Acquisition health: %u/%u valid, %.1f Hz, ok=%lu err=%lu",
               (unsigned)f.validCount, (unsigned)kSensorCount,
               (double)h.measuredRateHz, (unsigned long)ok, (unsigned long)err);
    }
  }
  // Refresh the cached high-rate status on changes and once per second.
  static uint8_t oledOnlineMask = 0xFF;
  static uint32_t lastOledHealthMs = 0;
  if (gpSensors->configuredRateHz() >= 50 && gpSensors->latest(f)) {
    uint8_t onlineMask = 0;
    for (unsigned i = 0; i < kSensorCount; ++i) {
      const auto& r = f.sensors[i];
      if (r.valid && r.status == SensorStatus::OK && !r.saturated && millis()-r.timestampMs<=1000) onlineMask |= 1u << i;
    }
    if (onlineMask != oledOnlineMask || millis() - lastOledHealthMs >= 1000) {
      HealthReport h;
      gpSensors->health(h);
      gOled.setSensorFrame(f);
      gOled.setHealth(h);
      gOled.show(gPhone.connected()?OledScreen::MEASUREMENT:OledScreen::AP_SETUP);
      oledOnlineMask = onlineMask;
      lastOledHealthMs = millis();
    }
  }
  // Also publish stale/error transitions when acquisition stops producing frames.
  if (millis() - lastWsMs >= kWsSensorIntervalMs && gpSensors->latest(f)) {
    lastWsMs=millis();
    gWs.broadcastSensors(f,ClassificationEstimate{},gpSensors->measuredRateHz());
  }
  // Progress queued display transfers even at the maximum acquisition rate.
  gOled.update(gpSensors->configuredRateHz() >= 50);
}
