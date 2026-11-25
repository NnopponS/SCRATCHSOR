#pragma once
// ESP32 DevKit 30-pin (USB-C / CH9102X) pin map for SCRATCHSOR.
// Only the primary I2C bus is wired; the MLX90393 INT pins are not connected.

namespace scratchsor {

inline constexpr int kPinI2cSda = 21;  // -> PCA9548A SDA
inline constexpr int kPinI2cScl = 22;  // -> PCA9548A SCL

}  // namespace scratchsor
