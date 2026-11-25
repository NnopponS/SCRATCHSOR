#pragma once
// Shared vocabulary types used across every SCRATCHSOR module.
// Keep this file free of module headers — it may be included anywhere.

#include <Arduino.h>
#include <cstdint>
#include "config.h"
#include "services/gap_policy.h"
#include "services/fingerprint.h"

// esp32-hal-gpio.h defines DISABLED/ENABLED as macros which collide with our
// enum constants below.
#ifdef DISABLED
#undef DISABLED
#endif
#ifdef ENABLED
#undef ENABLED
#endif

namespace scratchsor {

// ---- Enumerations ----

enum class WifiMode : uint8_t { AP_ONLY = 0, STA_ONLY = 1, AP_STA = 2 };

enum class WifiStaState : uint8_t {
  IDLE = 0, CONNECTING, CONNECTED, FAILED, DISABLED
};

enum class SensorStatus : uint8_t {
  UNKNOWN = 0, OK, NOT_FOUND, NACK, TIMEOUT, SATURATED, COMM_ERROR, DISABLED
};

enum class MeasurementState : uint8_t {
  NOT_CALIBRATED = 0, CALIBRATING, VALID, LOW_CONFIDENCE, OUT_OF_RANGE,
  SENSOR_ERROR, WARMING_UP
};

enum class OledController : uint8_t { UNKNOWN = 0, SSD1306, SH1106 };

enum class OledScreen : uint8_t {
  BOOT = 0, AP_INFO, AP_SETUP, STA_STATUS, SENSOR_HEALTH, READINGS, MEASUREMENT,
  LIVE_SUMMARY
};

// Short strings for serial/OLED/UI. Keep in sync with enums above.
const char* toString(SensorStatus s);
const char* toString(WifiStaState s);
const char* toString(MeasurementState s);

// ---- Sensor data ----

struct Vec3 {
  float x = 0, y = 0, z = 0;
  float magnitude() const;
};

struct SensorReading {
  uint8_t  sensorId = 0;        // logical index 0..3
  uint8_t  muxChannel = 0;      // mux channel it lives on
  uint8_t  i2cAddr = 0;         // resolved MLX90393 address (0 if absent)
  uint32_t seq = 0;             // frame sequence number
  uint32_t timestampMs = 0;     // millis() when the frame was assembled
  Vec3     raw;                 // microtesla, unfiltered
  Vec3     filtered;            // microtesla, after signal chain
  float    magnitudeRaw = 0;
  float    magnitudeFiltered = 0;
  float    temperatureC = 0;    // NaN when not acquired
  SensorStatus status = SensorStatus::UNKNOWN;
  bool     saturated = false;
  bool     valid = false;       // reading is fresh + status == OK
  float    quality = 0;         // 0..1 signal-quality estimate
};

// A complete (or partially failed) 4-sensor acquisition tick.
struct SensorFrame {
  uint32_t seq = 0;
  uint32_t timestampMs = 0;
  uint8_t  validCount = 0;
  SensorReading sensors[kSensorCount];
};

// ---- Wi-Fi ----

struct WifiStatus {
  WifiMode     mode = WifiMode::AP_ONLY;
  WifiStaState staState = WifiStaState::IDLE;
  String     apSsid;
  IPAddress  apIp;
  uint8_t    apClients = 0;
  String     staSsid;
  IPAddress  staIp;
  int8_t     staRssi = 0;
  String     staError;          // last failure reason, "" if none
};

// ---- Configuration (persisted via storage_service) ----

struct FilterConfig {
  bool    movingAvgEnabled = true;
  uint8_t movingAvgWindow  = 4;
  bool    medianEnabled    = false;
  uint8_t medianWindow     = 5;
  bool    lowpassEnabled   = true;
  float   lowpassAlpha     = 0.35f;   // 0..1, higher = less smoothing
  bool    baselineRemoval  = true;
  float   outlierSigma     = 4.0f;    // 0 disables outlier gate
};

struct SensorConfig {
  uint8_t gain         = 7;      // MLX90393 GAIN_SEL index 0..7 (7 = 1x)
  uint8_t resolution   = 0;      // RES_xyz index 0..3
  uint8_t oversampling = 0;      // short conversion for 50 Hz four-sensor frames
  uint8_t digitalFilter = 0;     // DIG_FILT 0..7; faster conversion trades noise
  float   sampleRateHz = kSampleRateHz;
  bool    readTemperature = true;
};

struct OledConfig {
  uint8_t refreshHz = kOledRefreshHz;
  uint8_t rotateSec = kOledRotateSec;
  bool    flipDisplay = false;
  uint8_t controllerOverride = 0;  // 0=auto, 1=SSD1306, 2=SH1106
};

struct DeviceConfig {
  uint32_t schema = kConfigSchema;
  char     deviceName[32] = "SCRATCHSOR";
  bool     apPersistent = true;   // keep SoftAP up as management interface
  SensorConfig sensor;
  FilterConfig filter;
  OledConfig   oled;
  GapConfig   gap;
  ReachConfig reach;
};

// ---- Calibration / measurement ----

// Feature vector captured at one known reference distance.
struct CalibrationPoint {
  float    referenceMm = 0;                 // physically measured distance
  float    baselineSubtractedMag[kSensorCount] = {0}; // |ΔB| per sensor, uT
  float    combinedMag = 0;                 // L2 norm across sensors
  float    spread[kSensorCount] = {0};      // stddev of collected samples
  uint16_t sampleCount = 0;
  uint32_t timestampMs = 0;
};

enum class CalibModelType : uint8_t {
  NONE = 0,
  // Linear fit on dipole-linearized feature u = (1/|ΔB|)^(1/3):
  //   d_mm = c0 + c1*u  (type DIPLOLE_LINEAR)
  //   d_mm = c0 + c1*u + c2*u^2 (DIPOLE_QUAD)
  DIPOLE_LINEAR = 1,
  DIPOLE_QUAD   = 2,
};

struct CalibrationModel {
  CalibModelType type = CalibModelType::NONE;
  float    coeffs[4] = {0};
  float    maeMm = 0;              // validation metrics (held-out data only)
  float    rmseMm = 0;
  float    maxErrMm = 0;
  float    minRangeMm = 0;
  float    maxRangeMm = 0;
  uint8_t  trainCount = 0;
  uint8_t  validCount = 0;
  bool     validated = false;      // true only after held-out validation passed
  uint32_t createdMs = 0;
  uint16_t crc = 0;                // integrity check for NVS blob
  uint32_t setupSignature = 0;
  uint8_t sensorMask = 0;
  Vec3 baseline[kSensorCount]{};
};

struct ClassificationEstimate {
  MeasurementState state = MeasurementState::NOT_CALIBRATED;
  int8_t category = -1;
  uint8_t profile = 0;
  bool selected = false;
  bool ready = false;
  float confidence = 0;            // 0..1
  uint32_t timestampMs = 0;
};

// Shared by collection and inference: reject incomplete, saturated or stale frames.
inline bool fingerprintFeatures(const SensorFrame& f, uint32_t now, float* out) {
  if (now - f.timestampMs > 1000 || f.validCount != kSensorCount) return false;
  for (unsigned i=0; i<kSensorCount; ++i) {
    const auto& r=f.sensors[i];
    if (!r.valid || r.status != SensorStatus::OK || r.saturated || now-r.timestampMs>1000) return false;
    out[i*3]=r.filtered.x;out[i*3+1]=r.filtered.y;out[i*3+2]=r.filtered.z;
    for(unsigned j=0;j<3;++j)if(!std::isfinite(out[i*3+j]))return false;
  }
  return true;
}

// ---- Diagnostics ----

struct SensorHealth {
  SensorStatus status = SensorStatus::UNKNOWN;
  uint8_t i2cAddr = 0;
  uint32_t okCount = 0;
  uint32_t errCount = 0;
  uint32_t lastErrorMs = 0;
};

struct HealthReport {
  uint32_t uptimeMs = 0;
  uint32_t freeHeap = 0;
  bool     muxFound = false;
  uint8_t  muxAddr = 0;
  bool     oledFound = false;
  uint8_t  oledAddr = 0;
  OledController oledController = OledController::UNKNOWN;
  SensorHealth sensor[kSensorCount];
  float    measuredRateHz = 0;     // effective 4-sensor frame rate
};

}  // namespace scratchsor
