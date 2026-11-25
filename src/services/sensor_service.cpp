// SCRATCHSOR — sensor acquisition service implementation.
// See services/sensor_service.h for the threading contract: the acquisition
// task is the only Mlx90393::read caller; snapshots publish under mux_;
// reconfiguration is deferred to a frame boundary via cfgPending_.

#include "services/sensor_service.h"
#include "hardware/i2c_manager.h"
#include "diagnostics/diagnostics.h"
#include "product/phone_product.h"
#include <new>
#include <cmath>

namespace scratchsor {

SensorService* gpSensors = nullptr;

SensorService::SensorService(I2CManager& bus) : bus_(bus) {}

uint8_t SensorService::begin(const SensorConfig& scfg, const FilterConfig& fcfg) {
  scfg_ = scfg;
  fcfg_ = fcfg;

  // Stream snapshots only; no trial buffer is allocated on the ESP32.

  uint8_t found = 0;
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    if (!sensors_[i]) {
      sensors_[i] = new (std::nothrow) Mlx90393(bus_, i, kMuxChSensor[i]);
    }
    if (!sensors_[i]) {
      SCR_LOGE("sensor %u allocation failed", (unsigned)i);
      continue;
    }
    if (sensors_[i]->begin(scfg_)) ++found;
    // Filter window sizes are compile-time capacities (MovingAverage<8>,
    // MedianFilter<9>) fixed by the frozen header; the runtime window fields
    // in FilterConfig are therefore advisory only.
    filters_[i].configure(fcfg_.medianEnabled, fcfg_.movingAvgEnabled,
                          fcfg_.lowpassEnabled, fcfg_.lowpassAlpha,
                          fcfg_.outlierSigma);
  }
  return found;
}

void SensorService::start() {
  if (task_) return;
  const BaseType_t ok = xTaskCreatePinnedToCore(
      taskThunk, "sensors", kAcquisitionStack, this,
      kAcquisitionPrio, &task_, kAcquisitionCore);
  if (ok != pdPASS) {
    task_ = nullptr;
    SCR_LOGE("acquisition task create failed");
  }
}

void SensorService::stop() {
  // Delete (rather than suspend) so a later start() never leaves a duplicate
  // task running. If the task was mid-I2C the bus mutex needs one
  // kI2cLockTimeoutMs to recover — acceptable on a restart path.
  if (task_) {
    TaskHandle_t t = task_;
    task_ = nullptr;
    vTaskDelete(t);
  }
}

void SensorService::taskThunk(void* arg) {
  static_cast<SensorService*>(arg)->run();
  vTaskDelete(nullptr);  // unreachable — run() loops forever
}

void SensorService::run() {
  for (;;) {
    TickType_t frameTick = xTaskGetTickCount();
    const uint32_t frameStart = millis();
    applyPendingBaselines();
    SensorFrame f;
    acquireFrame(f);
    gPhone.onFrame(f);

    // One short critical section covers snapshot publish + recorder append:
    // two bounded SensorFrame memcpys, no I2C, no allocation inside.
    portENTER_CRITICAL(&mux_);
    latest_ = f;
    haveFrame_ = true;
    if (recording_ && recBuf_) {
      if (!recording() || recCount_ >= kRecordMaxFrames) {
        recording_ = false;
      } else if (uint32_t(f.timestampMs - recStartedMs_) < kRecordDurationMs) {
        // Exclude a conversion that began before the Record command.
        recBuf_[recCount_++] = f;
        if (recCount_ == kRecordMaxFrames) recording_ = false;
      }
    }
    portEXIT_CRITICAL(&mux_);

    // Effective rate: EMA (alpha 0.1) over the instantaneous rate derived
    // from the measured inter-frame interval. Unsigned dt is wrap-safe.
    if (lastFrameMs_ != 0) {
      const uint32_t dt = f.timestampMs - lastFrameMs_;
      if (dt > 0) {
        const float inst = 1000.0f / (float)dt;
        rateEma_ = (rateEma_ <= 0.0f) ? inst
                                      : rateEma_ + 0.1f * (inst - rateEma_);
      }
    }
    lastFrameMs_ = f.timestampMs;

    // Deferred reconfiguration applied only at a frame boundary — never
    // mid-sample register churn on the MLX90393s.
    if (cfgPending_) applyPendingConfig();

    if (onlineCount() == 0) {
      // Degraded: nothing to poll. Keep publishing (stale NOT_FOUND frames
      // keep health/UI alive) but idle slowly to leave the bus free.
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // Pace to the configured frame rate: fixed per-frame budget minus work
    // already done, at least one tick so the watchdog/other tasks run.
    float rate = scfg_.sampleRateHz;
    if (rate < kSampleRateMinHz) rate = kSampleRateMinHz;
    if (rate > kSampleRateMaxHz) rate = kSampleRateMaxHz;
    const uint32_t budgetMs = (uint32_t)(1000.0f / rate);
    const uint32_t elapsed = millis() - frameStart;
    TickType_t ticks = pdMS_TO_TICKS(budgetMs);
    if (ticks < 1) ticks = 1;
    // An absolute tick deadline avoids rounding every frame one tick early.
    if (elapsed < budgetMs) {
      vTaskDelayUntil(&frameTick, ticks);
    } else {
      vTaskDelay(1);
    }
  }
}

void SensorService::acquireFrame(SensorFrame& f) {
  f.seq = latest_.seq + 1;        // single writer: only this task publishes
  f.timestampMs = millis();
  f.validCount = 0;
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    SensorReading& r = f.sensors[i];
    r = SensorReading{};
    r.sensorId = i;
    r.muxChannel = kMuxChSensor[i];
    r.seq = f.seq;
    r.timestampMs = f.timestampMs;

    Mlx90393* s = sensors_[i];
    if (!s || !s->present()) {
      r.status = SensorStatus::NOT_FOUND;
      r.valid = false;
      continue;
    }

    s->read(r);  // driver fills raw/temp/status/valid/saturated
    r.i2cAddr = s->addr();
    r.magnitudeRaw = r.raw.magnitude();  // recompute — don't rely on driver
    if (r.valid) {
      bool o = false;  // per-axis outlier flag; rejected axes hold last value
      r.filtered.x = filters_[i].x().update(r.raw.x, o);
      r.filtered.y = filters_[i].y().update(r.raw.y, o);
      r.filtered.z = filters_[i].z().update(r.raw.z, o);
      r.magnitudeFiltered = r.filtered.magnitude();
      ++f.validCount;
    }
    // Quality stays meaningful even on a failed read (noise history kept).
    r.quality = (filters_[i].x().quality() + filters_[i].y().quality() +
                 filters_[i].z().quality()) / 3.0f;
  }
}

// ------------------------- snapshot APIs -------------------------------

bool SensorService::latest(SensorFrame& out) const {
  portENTER_CRITICAL(&mux_);
  const bool have = haveFrame_;
  if (have) out = latest_;
  portEXIT_CRITICAL(&mux_);
  return have;
}

SensorReading SensorService::latest(uint8_t sensorId) const {
  SensorReading r{};
  if (sensorId < kSensorCount) {
    portENTER_CRITICAL(&mux_);
    if (haveFrame_) r = latest_.sensors[sensorId];
    portEXIT_CRITICAL(&mux_);
  }
  return r;
}

void SensorService::health(HealthReport& out) const {
  // The driver counters are written by read() on the acquisition task, which
  // does not hold mux_ during I2C — the copy below is consistent w.r.t.
  // snapshot readers but the counters themselves are simple u32s read at
  // worst one frame stale. Honest diagnostic data, not a hard interlock.
  portENTER_CRITICAL(&mux_);
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    if (sensors_[i] && sensors_[i]->present()) {
      out.sensor[i] = sensors_[i]->health();
      out.sensor[i].i2cAddr = sensors_[i]->addr();
      if (out.sensor[i].status == SensorStatus::UNKNOWN) {
        out.sensor[i].status = SensorStatus::OK;
      }
    } else {
      out.sensor[i] = SensorHealth{};
      out.sensor[i].status = SensorStatus::NOT_FOUND;
    }
  }
  out.measuredRateHz = rateEma_;
  portEXIT_CRITICAL(&mux_);
}

float SensorService::measuredRateHz() const {
  portENTER_CRITICAL(&mux_);
  const float r = rateEma_;
  portEXIT_CRITICAL(&mux_);
  return r;
}

uint8_t SensorService::onlineCount() const {
  // present() is immutable after begin() — no lock needed for this count.
  uint8_t n = 0;
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    if (sensors_[i] && sensors_[i]->present()) ++n;
  }
  return n;
}

// --------------------- runtime reconfiguration -------------------------

void SensorService::requestConfig(const SensorConfig& scfg,
                                  const FilterConfig& fcfg) {
  // Copy under the lock, then flag — the acquisition task applies at the
  // next frame boundary in applyPendingConfig().
  portENTER_CRITICAL(&mux_);
  nextScfg_ = scfg;
  nextFcfg_ = fcfg;
  cfgPending_ = true;
  portEXIT_CRITICAL(&mux_);
}

void SensorService::applyPendingConfig() {
  SensorConfig s;
  FilterConfig f;
  portENTER_CRITICAL(&mux_);
  s = nextScfg_;
  f = nextFcfg_;
  cfgPending_ = false;
  portEXIT_CRITICAL(&mux_);

  scfg_ = s;
  fcfg_ = f;
  // I2C register writes and filter resets happen OUTSIDE the critical
  // section — applyConfig is a slow multi-transfer operation.
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    if (sensors_[i] && sensors_[i]->present()) sensors_[i]->applyConfig(s);
    // configure() fully resets the chain, so any stored baseline is dropped:
    // a changed filter chain invalidates the old baseline anyway.
    filters_[i].configure(f.medianEnabled, f.movingAvgEnabled,
                          f.lowpassEnabled, f.lowpassAlpha, f.outlierSigma);
  }
}

void SensorService::setBaselines(const Vec3 b[kSensorCount]) {
  portENTER_CRITICAL(&mux_);
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    nextBaselines_[i] = b[i];
  }
  baselinePending_ = true;
  portEXIT_CRITICAL(&mux_);
}

void SensorService::clearBaselines() {
  const Vec3 zero[kSensorCount]{};
  setBaselines(zero);
}

void SensorService::applyPendingBaselines() {
  Vec3 b[kSensorCount];
  portENTER_CRITICAL(&mux_);
  const bool pending = baselinePending_;
  if (pending) {
    for (uint8_t i = 0; i < kSensorCount; ++i) b[i] = nextBaselines_[i];
    baselinePending_ = false;
  }
  portEXIT_CRITICAL(&mux_);
  if (!pending) return;
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    if (fcfg_.baselineRemoval) filters_[i].setBaseline(b[i].x, b[i].y, b[i].z);
    else filters_[i].clearBaseline();
  }
}

// --------------------------- recorder ----------------------------------

bool SensorService::recordStart(float referenceMm) {
  if (!std::isfinite(referenceMm) || referenceMm < -500 || referenceMm > 500) return false;
  SensorFrame frame;
  if (!latest(frame) || !frame.validCount || uint32_t(millis() - frame.timestampMs) > 1000) return false;
  portENTER_CRITICAL(&mux_);
  if (recording() || !recBuf_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  recHead_ = 0;
  recCount_ = 0;
  recReferenceMm_ = referenceMm;
  recStartedMs_ = millis();
  recording_ = true;
  portEXIT_CRITICAL(&mux_);
  return true;
}

void SensorService::recordStop() {
  portENTER_CRITICAL(&mux_);
  recording_ = false;
  portEXIT_CRITICAL(&mux_);
}

uint16_t SensorService::recordCount() const {
  portENTER_CRITICAL(&mux_);
  const uint16_t n = recCount_;
  portEXIT_CRITICAL(&mux_);
  return n;
}

uint16_t SensorService::recordCopy(SensorFrame* out, uint16_t max, uint16_t offset) {
  if (!out || !recBuf_) return 0;
  portENTER_CRITICAL(&mux_);
  const uint16_t available = !recording() && offset < recCount_ ? recCount_ - offset : 0;
  const uint16_t n = (max < available) ? max : available;
  for (uint16_t k = 0; k < n; ++k) {
    out[k] = recBuf_[(uint16_t)((recHead_ + offset + k) % kRecordMaxFrames)];
  }
  portEXIT_CRITICAL(&mux_);
  return n;
}

}  // namespace scratchsor
