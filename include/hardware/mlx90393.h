#pragma once
// MLX90393 driver — one logical instance per physical sensor, each bound to
// its own PCA9548A channel so internal state never leaks between sensors.
//
// Protocol (datasheet): commands are single byte; RM prepends a status byte
// then returns 2 bytes per requested axis in order T,X,Y,Z (each enabled bit
// adds two bytes). SM starts a single-shot conversion; data ready after
// t_conv = f(OSR, DIG_FILT, resolution) — computed in conversionTimeMs().
//
// CJMCU-90393 breakout notes (docs/wiring.md): CS must be HIGH for I2C mode;
// most boards tie CS high via on-board pull-up — verified empirically by
// successful comms at boot and recorded in diagnostics.

#include <cstdint>
#include <cstddef>
#include "config.h"
#include "device_types.h"

namespace scratchsor {

class I2CManager;

// Axis bits inside the ZYXT nibble of SM/RM commands.
namespace mlx {
inline constexpr uint8_t kAxisT = 0x01;
inline constexpr uint8_t kAxisX = 0x02;
inline constexpr uint8_t kAxisY = 0x04;
inline constexpr uint8_t kAxisZ = 0x08;
inline constexpr uint8_t kAxisXYZ  = kAxisX | kAxisY | kAxisZ;
inline constexpr uint8_t kAxisXYZT = kAxisXYZ | kAxisT;

// Command opcodes (upper nibble) — datasheet table "command set".
inline constexpr uint8_t kCmdRT = 0xF0;  // reset
inline constexpr uint8_t kCmdEX = 0x80;  // exit (abort)
inline constexpr uint8_t kCmdSB = 0x10;  // burst mode
inline constexpr uint8_t kCmdSW = 0x20;  // wake-on-change
inline constexpr uint8_t kCmdSM = 0x30;  // single measurement
inline constexpr uint8_t kCmdRM = 0x40;  // read measurement
inline constexpr uint8_t kCmdRR = 0x50;  // read register
inline constexpr uint8_t kCmdWR = 0x60;  // write register

// Status byte bits.
inline constexpr uint8_t kStatBurstData = 0x80;
inline constexpr uint8_t kStatWocMode   = 0x40;
inline constexpr uint8_t kStatSMMode    = 0x20;
inline constexpr uint8_t kStatError     = 0x10;
inline constexpr uint8_t kStatSED       = 0x08;
inline constexpr uint8_t kStatRS        = 0x04;  // reset occurred
inline constexpr uint8_t kStatD1        = 0x02;
inline constexpr uint8_t kStatD0        = 0x01;  // D1:D0 = new-data axes

// Register addresses written via WR/RR.
inline constexpr uint8_t kRegGainRes   = 0x00;  // GAIN_SEL, RES_xyz, DIG_FILT, OSR
inline constexpr uint8_t kRegResOsrdig = 0x02;  // per datasheet map: 0x00..0x02
inline constexpr uint8_t kRegTcmp      = 0x01;  // TCMP_EN, COMM_MODE etc.
}  // namespace mlx

class Mlx90393 {
public:
  Mlx90393(I2CManager& bus, uint8_t sensorId, uint8_t muxChannel)
      : bus_(bus), sensorId_(sensorId), muxChannel_(muxChannel) {}

  // Probe 0x0C..0x0F on this sensor's channel, reset the part, apply cfg.
  // Returns true when the sensor ACKs and accepts configuration.
  bool begin(const SensorConfig& cfg);

  bool present() const { return addr_ != 0; }
  uint8_t addr() const { return addr_; }
  uint8_t muxChannel() const { return muxChannel_; }
  uint8_t sensorId() const { return sensorId_; }

  // Single-shot XYZ(+T) measurement. Blocks up to conversionTimeMs()+margin.
  // On success fills `out` (µT + °C) and sets out->status=OK, valid=true.
  // On failure sets a precise status (NACK/TIMEOUT/SATURATED/COMM_ERROR),
  // valid=false, and bumps error counters used by diagnostics.
  bool read(SensorReading& out);

  // Re-apply config (gain/res/OSR/digfilt). May be called at runtime.
  bool applyConfig(const SensorConfig& cfg);

  // Recovery path after repeated errors: RT reset + re-apply config.
  bool recover();

  // Estimated single-conversion time for current settings (datasheet formula).
  uint32_t conversionTimeMs() const;

  SensorHealth& health() { return health_; }

private:
  bool exchange(const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxLen);
  bool sm(uint8_t axes);                    // start measurement
  bool rm(uint8_t axes, uint16_t* raw, uint8_t& status); // read raw words
  bool writeReg(uint8_t reg, uint16_t value);
  bool readReg(uint8_t reg, uint16_t& value);
  bool reset();
  float lsbPerUtXY() const;                 // µT per LSB for X/Y at cfg_
  float lsbPerUtZ() const;                  // µT per LSB for Z at cfg_
  void markErr(SensorStatus s);

  I2CManager& bus_;
  uint8_t sensorId_;
  uint8_t muxChannel_;
  uint8_t addr_ = 0;
  SensorConfig cfg_{};
  SensorHealth health_{};
  uint32_t consecErr_ = 0;
};

}  // namespace scratchsor
