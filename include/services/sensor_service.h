#pragma once
// Sensor acquisition service: owns 4 Mlx90393 instances (one per mux
// channel), a dedicated FreeRTOS acquisition task on the low-priority core,
// the per-sensor filter chains, the latest-frame data model, and the
// measured effective frame rate.
//
// Threading contract:
//  - Acquisition task (core kAcquisitionCore) is the ONLY caller of
//    Mlx90393::read. It publishes complete SensorFrame snapshots under a
//    short critical section.
//  - Consumers call latest()/health()/measuredRateHz() — lock-protected,
//    never stale-marked-as-fresh: each reading carries its own timestamp.
//  - Reconfiguration goes through an atomic pending-config flag applied at
//    the next frame boundary — no mid-sample register churn.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "device_types.h"
#include "hardware/mlx90393.h"
#include "services/signal_processing.h"

namespace scratchsor {

class I2CManager;

class SensorService {
public:
  explicit SensorService(I2CManager& bus);

  // Construct sensors + filters; does NOT start the task. Returns count of
  // sensors found on the bus (0 is a legal degraded state).
  uint8_t begin(const SensorConfig& scfg, const FilterConfig& fcfg);

  // Launch acquisition task. Safe to call even with 0 sensors (task idles,
  // health stays visible).
  void start();

  void stop();   // for tests/restart — suspends task

  // Snapshot APIs (consumer-side, short lock).
  bool latest(SensorFrame& out) const;         // false if no frame yet
  SensorReading latest(uint8_t sensorId) const;
  void health(HealthReport& out) const;
  float measuredRateHz() const;
  float configuredRateHz() const { return scfg_.sampleRateHz; }
  uint8_t onlineCount() const;

  // Runtime reconfiguration — deferred to next frame boundary.
  void requestConfig(const SensorConfig& scfg, const FilterConfig& fcfg);

  // Baseline control (calibration service calls after baseline capture).
  void setBaselines(const Vec3 b[kSensorCount]);
  void clearBaselines();

  // Bounded RAM recorder for export (started/stopped via API/WS).
  bool recordStart(float referenceMm);
  void recordStop();
  bool recording() const { return recording_ && uint32_t(millis() - recStartedMs_) < kRecordDurationMs; }
  float recordReferenceMm() const { return recReferenceMm_; }
  uint32_t recordStartedMs() const { return recStartedMs_; }
  uint32_t recordRemainingMs() const { const uint32_t elapsed = millis() - recStartedMs_; return recording_ && elapsed < kRecordDurationMs ? kRecordDurationMs - elapsed : 0; }
  uint16_t recordCount() const;
  // Copy retained frames from offset. Stop recording before exporting.
  uint16_t recordCopy(SensorFrame* out, uint16_t max, uint16_t offset = 0);

private:
  static void taskThunk(void* arg);
  void run();                  // task body
  void acquireFrame(SensorFrame& f);
  void applyPendingConfig();
  void applyPendingBaselines();

  I2CManager& bus_;
  Mlx90393* sensors_[kSensorCount] = {};
  Vec3Filter filters_[kSensorCount];
  SensorConfig scfg_{};
  FilterConfig fcfg_{};

  TaskHandle_t task_ = nullptr;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  SensorFrame latest_{};
  bool haveFrame_ = false;
  float rateEma_ = 0;
  uint32_t lastFrameMs_ = 0;

  volatile bool cfgPending_ = false;
  SensorConfig nextScfg_{};
  FilterConfig nextFcfg_{};
  bool baselinePending_ = false;
  Vec3 nextBaselines_[kSensorCount]{};

  volatile bool recording_ = false;
  uint32_t recStartedMs_ = 0;
  float recReferenceMm_ = 0;
  SensorFrame* recBuf_ = nullptr;
  uint16_t recHead_ = 0, recCount_ = 0;
};

extern SensorService* gpSensors;  // created in main (needs heap lifetime)

}  // namespace scratchsor
