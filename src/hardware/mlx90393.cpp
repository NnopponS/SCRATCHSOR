// Mlx90393 — one driver instance per physical sensor, each bound to its own
// PCA9548A channel. Every transaction takes a fresh I2CBusGuard on muxChannel_
// (the guard re-selects the channel — nothing persists between ops).
//
// Protocol sources (MLX90393 datasheet, Melexis rev. >=010):
//   https://www.melexis.com/-/media/files/documents/datasheets/mlx90393-datasheet-melexis.pdf
// Reference driver cross-checked against:
//   https://github.com/adafruit/Adafruit_MLX90393_Library (master)

#include "hardware/mlx90393.h"
#include "hardware/i2c_manager.h"
#include "diagnostics/diagnostics.h"
#include <math.h>

namespace scratchsor {

namespace {

// Register-address byte inside RR/WR commands is the memory address shifted
// left by 2 (datasheet: address occupies bits [7:2] of the last command byte).
constexpr uint8_t regAddr(uint8_t reg) { return (uint8_t)(reg << 2); }

// Magnetic sensitivity in uT/LSB, indexed [GAIN_SEL][RES_XYZ][0=XY, 1=Z],
// valid for HALLCONF=0xC (the power-on default we preserve via RMW of reg 0x00).
// Values: MLX90393 datasheet Table 17 (post-2020 revision — sensitivity now
// depends on HALLCONF); identical to mlx90393_lsb_lookup[0] in
// Adafruit_MLX90393.h (library >= 1.1.1, github.com/adafruit/
// Adafruit_MLX90393_Library). Older tables ({0.805,1.468,...} base) came from
// a 2015 app note and are obsolete.
constexpr float kSensUtPerLsb[8][4][2] = {
    /* GAIN_SEL=0 (5x)   */ {{0.751f, 1.210f}, {1.502f, 2.420f},
                             {3.004f, 4.840f}, {6.009f, 9.680f}},
    /* GAIN_SEL=1 (4x)   */ {{0.601f, 0.968f}, {1.202f, 1.936f},
                             {2.403f, 3.872f}, {4.840f, 7.744f}},
    /* GAIN_SEL=2 (3x)   */ {{0.451f, 0.726f}, {0.901f, 1.452f},
                             {1.803f, 2.904f}, {3.605f, 5.808f}},
    /* GAIN_SEL=3 (2.5x) */ {{0.376f, 0.605f}, {0.751f, 1.210f},
                             {1.502f, 2.420f}, {3.004f, 4.840f}},
    /* GAIN_SEL=4 (2x)   */ {{0.300f, 0.484f}, {0.601f, 0.968f},
                             {1.202f, 1.936f}, {2.403f, 3.872f}},
    /* GAIN_SEL=5 (1.67x)*/ {{0.250f, 0.403f}, {0.501f, 0.807f},
                             {1.001f, 1.613f}, {2.003f, 3.227f}},
    /* GAIN_SEL=6 (1.33x)*/ {{0.200f, 0.323f}, {0.401f, 0.645f},
                             {0.801f, 1.291f}, {1.602f, 2.581f}},
    /* GAIN_SEL=7 (1x)   */ {{0.150f, 0.242f}, {0.300f, 0.484f},
                             {0.601f, 0.968f}, {1.202f, 1.936f}},
};

// Single-conversion time in ms, indexed [DIG_FILT][OSR] — datasheet Table 18
// (also mlx90393_tconv in Adafruit_MLX90393.h and _TCONV_LOOKUP in
// Adafruit_CircuitPython_MLX90393/adafruit_mlx90393.py).
constexpr float kTconvMs[8][4] = {
    /* DIG_FILT=0 */ {1.27f, 1.84f, 3.00f, 5.30f},
    /* DIG_FILT=1 */ {1.46f, 2.23f, 3.76f, 6.84f},
    /* DIG_FILT=2 */ {1.84f, 3.00f, 5.30f, 9.91f},
    /* DIG_FILT=3 */ {2.61f, 4.53f, 8.37f, 16.05f},
    /* DIG_FILT=4 */ {4.15f, 7.60f, 14.52f, 28.34f},
    /* DIG_FILT=5 */ {7.22f, 13.75f, 26.80f, 52.92f},
    /* DIG_FILT=6 */ {13.36f, 26.04f, 51.38f, 102.07f},
    /* DIG_FILT=7 */ {25.65f, 50.61f, 100.53f, 200.37f},
};

// Temperature model — datasheet Table 7: TRES = 45.2 LSB/degC, output at
// 25 degC = 46244 LSB16u. T_C = (T_digital - 46244) / 45.2 + 25.
constexpr float kTempOffsetLsb = 46244.0f;
constexpr float kTempLsbPerC = 45.2f;
constexpr float kTempRefC = 25.0f;

// Register 0x00 bit fields (datasheet memory map): GAIN_SEL[6:4], HALLCONF[3:0]
// stays at its 0xC power-on value via read-modify-write.
constexpr uint16_t kGainMask = 0x0070u;
// Register 0x02: OSR[1:0], DIG_FILT[4:2], RES_X[6:5], RES_Y[8:7], RES_Z[10:9].
constexpr uint16_t kResOsrFiltMask =
    (0x3u << 5) | (0x3u << 7) | (0x3u << 9) | (0x7u << 2) | 0x3u;

constexpr uint8_t countAxes(uint8_t axes) {
  return (uint8_t)(((axes >> 0) & 1) + ((axes >> 1) & 1) + ((axes >> 2) & 1) +
                   ((axes >> 3) & 1));
}

// 16-bit magnetic word at a rail = ADC saturated / field beyond range.
inline bool wordSaturated(uint16_t w) {
  return w == 0x7FFF || w == 0x8000 || w == 0x8001;  // +32767, -32768, -32767
}

}  // namespace

bool Mlx90393::begin(const SensorConfig& cfg) {
  cfg_ = cfg;
  addr_ = 0;
  {
    I2CBusGuard guard(bus_, muxChannel_);
    if (!guard.ok()) {
      health_.status = SensorStatus::TIMEOUT;
      return false;
    }
    for (uint8_t a = kMlxAddrMin; a <= kMlxAddrMax; ++a) {
      if (bus_.probe(a)) {
        addr_ = a;
        break;
      }
    }
  }
  if (!addr_) {
    health_.status = SensorStatus::NOT_FOUND;
    health_.i2cAddr = 0;
    return false;
  }
  health_.i2cAddr = addr_;
  if (!reset() || !applyConfig(cfg)) {
    health_.status = SensorStatus::COMM_ERROR;
    return false;
  }
  consecErr_ = 0;
  health_.status = SensorStatus::OK;
  SCR_LOGI("MLX90393 #%u: 0x%02X on mux ch%u", (unsigned)sensorId_,
           (unsigned)addr_, (unsigned)muxChannel_);
  return true;
}

bool Mlx90393::exchange(const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxLen) {
  I2CBusGuard guard(bus_, muxChannel_);
  if (!guard.ok()) return false;
  // MLX commands and responses are separate STOP-terminated transactions,
  // as in Adafruit_MLX90393::transceive (not a register-style repeated START).
  // https://github.com/adafruit/Adafruit_MLX90393_Library
  if (!bus_.write(addr_, tx, txLen)) return false;
  delayMicroseconds(100);
  return bus_.read(addr_, rx, rxLen);
}

bool Mlx90393::sm(uint8_t axes) {
  const uint8_t cmd = (uint8_t)(mlx::kCmdSM | (axes & 0x0F));
  uint8_t status = 0;
  if (!exchange(&cmd, 1, &status, 1)) return false;
  // SM echoes a status byte; only the ERROR bit is a hard failure (RS may still
  // be set right after a reset, D1:D0 advertise stale data — both tolerated).
  return (status & mlx::kStatError) == 0;
}

bool Mlx90393::rm(uint8_t axes, uint16_t* raw, uint8_t& status) {
  axes &= 0x0F;
  const uint8_t cmd = (uint8_t)(mlx::kCmdRM | axes);
  const uint8_t need = (uint8_t)(1 + 2 * countAxes(axes));  // status + 2B/axis
  uint8_t buf[9];                                           // max: T+X+Y+Z
  if (!exchange(&cmd, 1, buf, need)) return false;
  status = buf[0];
  // Enabled axes stream MSB-first in fixed order T,X,Y,Z -> raw[0..3].
  uint8_t i = 1;
  if (axes & mlx::kAxisT) { raw[0] = (uint16_t)((buf[i] << 8) | buf[i + 1]); i += 2; }
  if (axes & mlx::kAxisX) { raw[1] = (uint16_t)((buf[i] << 8) | buf[i + 1]); i += 2; }
  if (axes & mlx::kAxisY) { raw[2] = (uint16_t)((buf[i] << 8) | buf[i + 1]); i += 2; }
  if (axes & mlx::kAxisZ) { raw[3] = (uint16_t)((buf[i] << 8) | buf[i + 1]); }
  return true;
}

bool Mlx90393::reset() {
  const uint8_t cmd = mlx::kCmdRT;
  bool ok;
  {
    I2CBusGuard guard(bus_, muxChannel_);
    if (!guard.ok()) return false;
    ok = bus_.write(addr_, &cmd, 1);  // RT: command only, no response bytes
  }
  // Datasheet: reset completes within ~1.5 ms; first status afterwards carries
  // the RS bit, which our status checks deliberately ignore.
  if (ok) delay(2);
  return ok;
}

bool Mlx90393::writeReg(uint8_t reg, uint16_t value) {
  const uint8_t tx[4] = {mlx::kCmdWR, (uint8_t)(value >> 8),
                         (uint8_t)(value & 0xFF), regAddr(reg)};
  uint8_t status = 0;
  if (!exchange(tx, sizeof(tx), &status, 1)) return false;
  return (status & mlx::kStatError) == 0;
}

bool Mlx90393::readReg(uint8_t reg, uint16_t& value) {
  const uint8_t tx[2] = {mlx::kCmdRR, regAddr(reg)};
  uint8_t rx[3] = {0};
  if (!exchange(tx, sizeof(tx), rx, sizeof(rx))) return false;
  if (rx[0] & mlx::kStatError) return false;
  value = (uint16_t)((rx[1] << 8) | rx[2]);
  return true;
}

bool Mlx90393::applyConfig(const SensorConfig& cfg) {
  if (!addr_) return false;
  cfg_ = cfg;

  // Reg 0x00: GAIN_SEL[6:4]. RMW preserves HALLCONF[3:0]=0xC and Z_SERIES/BIST.
  uint16_t r0 = 0;
  if (!readReg(mlx::kRegGainRes, r0)) return false;
  r0 = (uint16_t)((r0 & ~kGainMask) |
                  (((uint16_t)(cfg.gain & 0x07)) << 4));
  if (!writeReg(mlx::kRegGainRes, r0)) return false;

  // Reg 0x02: OSR[1:0], DIG_FILT[4:2], RES_X[6:5], RES_Y[8:7], RES_Z[10:9].
  const uint16_t res = (uint16_t)(cfg.resolution & 0x03);
  uint16_t r2 = 0;
  if (!readReg(mlx::kRegResOsrdig, r2)) return false;
  r2 = (uint16_t)((r2 & ~kResOsrFiltMask) | (res << 5) | (res << 7) |
                  (res << 9) | (((uint16_t)(cfg.digitalFilter & 0x07)) << 2) |
                  ((uint16_t)(cfg.oversampling & 0x03)));
  if (!writeReg(mlx::kRegResOsrdig, r2)) return false;
  return true;
}

uint32_t Mlx90393::conversionTimeMs() const {
  const uint8_t filt = cfg_.digitalFilter & 0x07;
  const uint8_t osr = cfg_.oversampling & 0x03;
  // Datasheet Table 18 value + ~20% margin (Adafruit C++ needed +10 ms, the
  // CircuitPython driver uses +10%; 20% + 1 ms floor sits between them).
  return (uint32_t)(kTconvMs[filt][osr] * 1.2f) + 1;
}

float Mlx90393::lsbPerUtXY() const {
  return kSensUtPerLsb[cfg_.gain & 0x07][cfg_.resolution & 0x03][0];
}

float Mlx90393::lsbPerUtZ() const {
  return kSensUtPerLsb[cfg_.gain & 0x07][cfg_.resolution & 0x03][1];
}

void Mlx90393::markErr(SensorStatus s) {
  health_.status = s;
  health_.errCount++;
  health_.lastErrorMs = millis();
  if (++consecErr_ >= 3 && addr_) {
    consecErr_ = 0;
    SCR_LOGW("MLX90393 #%u: 3 consecutive errors, recovering", (unsigned)sensorId_);
    recover();
  }
}

bool Mlx90393::recover() {
  if (!addr_) return false;
  bool ok = reset() && applyConfig(cfg_);
  if (ok) {
    consecErr_ = 0;
    health_.status = SensorStatus::OK;
    SCR_LOGI("MLX90393 #%u: recovered", (unsigned)sensorId_);
  }
  return ok;
}

bool Mlx90393::read(SensorReading& out) {
  out.sensorId = sensorId_;
  out.muxChannel = muxChannel_;
  out.i2cAddr = addr_;
  out.temperatureC = NAN;
  out.valid = false;
  out.saturated = false;

  if (!addr_) {
    out.status = SensorStatus::NOT_FOUND;
    markErr(SensorStatus::NOT_FOUND);
    return false;
  }

  const uint8_t axes =
      cfg_.readTemperature ? mlx::kAxisXYZT : mlx::kAxisXYZ;

  if (!sm(axes)) {
    out.status = SensorStatus::COMM_ERROR;
    markErr(SensorStatus::COMM_ERROR);
    return false;
  }

  delay(conversionTimeMs());  // single-shot: poll-free wait for DRDY

  uint16_t raw[4] = {0};  // [0]=T [1]=X [2]=Y [3]=Z
  uint8_t status = 0;
  if (!rm(axes, raw, status) || (status & mlx::kStatError)) {
    out.status = SensorStatus::COMM_ERROR;
    markErr(SensorStatus::COMM_ERROR);
    return false;
  }

  // Magnetic words are signed except RES_XYZ>=2, where the 16-bit readout
  // window shifts into the upper ADC bits and comes out unsigned — datasheet
  // requires re-basing (same correction as Adafruit_MLX90393::readMeasurement):
  //   RES=2 (18-bit): subtract 0x8000;  RES=3 (19-bit): subtract 0x4000.
  int32_t xi, yi, zi;
  if (cfg_.resolution == 2) {
    xi = (int32_t)raw[1] - 0x8000;
    yi = (int32_t)raw[2] - 0x8000;
    zi = (int32_t)raw[3] - 0x8000;
  } else if (cfg_.resolution == 3) {
    xi = (int32_t)raw[1] - 0x4000;
    yi = (int32_t)raw[2] - 0x4000;
    zi = (int32_t)raw[3] - 0x4000;
  } else {
    xi = (int16_t)raw[1];
    yi = (int16_t)raw[2];
    zi = (int16_t)raw[3];
  }

  const float xy = lsbPerUtXY();
  const float zz = lsbPerUtZ();
  out.raw.x = (float)xi * xy;
  out.raw.y = (float)yi * xy;
  out.raw.z = (float)zi * zz;
  out.magnitudeRaw = out.raw.magnitude();

  if (cfg_.readTemperature) {
    out.temperatureC =
        kTempRefC + ((float)raw[0] - kTempOffsetLsb) / kTempLsbPerC;
  }

  const bool sat =
      wordSaturated(raw[1]) || wordSaturated(raw[2]) || wordSaturated(raw[3]);
  out.saturated = sat;
  out.status = sat ? SensorStatus::SATURATED : SensorStatus::OK;
  out.quality = sat ? 0.2f : 1.0f;
  out.valid = true;

  consecErr_ = 0;
  health_.okCount++;
  health_.status = out.status;
  return true;
}

}  // namespace scratchsor
