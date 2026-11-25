// I2CManager — sole owner of the primary Wire bus. See i2c_manager.h for the
// transaction contract (lock -> channel select -> bounded transfer).

#include "hardware/i2c_manager.h"
#include "pins.h"
#include "diagnostics/diagnostics.h"

namespace scratchsor {

I2CManager gI2c;

// ESP32 TwoWire FIFO buffer is 128 bytes; reject larger transfers up front.
static constexpr size_t kWireMaxChunk = 128;

bool I2CManager::begin() {
  if (begun_) return true;
  if (!mutex_) {
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
      SCR_LOGE("I2C: mutex creation failed");
      return false;
    }
  }
  Wire.begin(kPinI2cSda, kPinI2cScl);
  Wire.setClock(kI2cClockHz);
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP32)
  Wire.setTimeOut(50);  // ms per transaction — bounds a stuck bus
#endif
  currentChannel_ = 0xFF;
  begun_ = true;
  SCR_LOGI("I2C up: SDA=%d SCL=%d @ %u Hz", kPinI2cSda, kPinI2cScl,
           (unsigned)kI2cClockHz);
  return true;
}

bool I2CManager::lock(uint32_t timeoutMs) {
  if (!mutex_) {
    // Tolerate a guard taken before begin() completed — the mutex is cheap.
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;
  }
  return xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void I2CManager::unlock() {
  if (mutex_) xSemaphoreGive(mutex_);
}

uint8_t I2CManager::detectMux() {
  // Lock-only guard: the mux sits on the primary bus, no channel selected.
  I2CBusGuard guard(*this, kI2cLockTimeoutMs);
  if (!guard.ok()) return 0;
  for (uint8_t a = kMuxAddrBase; a <= kMuxAddrMax; ++a) {
    if (probe(a)) {
      muxAddr_ = a;
      muxFound_ = true;
      currentChannel_ = 0xFF;  // selection state unknown after scan
      return a;
    }
  }
  muxAddr_ = 0;
  muxFound_ = false;
  return 0;
}

bool I2CManager::selectChannel(uint8_t channel) {
  if (!muxFound_ || muxAddr_ == 0 || channel >= kMuxChannels) return false;
  // PCA9548A control register: one bit per channel, single-byte write.
  if (writeByte(muxAddr_, (uint8_t)(1u << channel))) {
    currentChannel_ = channel;
    return true;
  }
  currentChannel_ = 0xFF;  // NACKed select — downstream state unknown
  return false;
}

bool I2CManager::deselectAll() {
  if (!muxFound_ || muxAddr_ == 0) return false;
  if (writeByte(muxAddr_, 0x00)) {
    currentChannel_ = 0xFF;
    return true;
  }
  return false;
}

bool I2CManager::probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool I2CManager::withChannel(uint8_t channel, const std::function<bool()>& fn,
                             uint32_t timeoutMs) {
  if (!lock(timeoutMs)) return false;
  bool ok = selectChannel(channel);
  if (ok && fn) ok = fn();
  unlock();
  return ok;
}

bool I2CManager::write(uint8_t addr, const uint8_t* data, size_t len,
                       bool sendStop) {
  if (!data || len == 0 || len > kWireMaxChunk) return false;
  Wire.beginTransmission(addr);
  if (Wire.write(data, len) != len) {
    Wire.endTransmission();
    return false;
  }
  return Wire.endTransmission(sendStop) == 0;
}

bool I2CManager::writeByte(uint8_t addr, uint8_t value) {
  return write(addr, &value, 1);
}

bool I2CManager::read(uint8_t addr, uint8_t* buf, size_t len) {
  if (!buf || len == 0 || len > kWireMaxChunk) return false;
  size_t got = Wire.requestFrom((uint16_t)addr, (uint8_t)len);  // STOP after read
  if (got != len) {
    while (Wire.available()) (void)Wire.read();  // drain partial frame
    return false;
  }
  for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t)Wire.read();
  return true;
}

bool I2CManager::writeRead(uint8_t addr, const uint8_t* w, size_t wlen,
                           uint8_t* r, size_t rlen) {
  if (!w || wlen == 0 || !r || rlen == 0 || wlen > kWireMaxChunk ||
      rlen > kWireMaxChunk) {
    return false;
  }
  Wire.beginTransmission(addr);
  if (Wire.write(w, wlen) != wlen) {
    Wire.endTransmission();
    return false;
  }
  if (Wire.endTransmission(false) != 0) return false;  // repeated START
  size_t got = Wire.requestFrom((uint16_t)addr, (uint8_t)rlen);
  if (got != rlen) {
    while (Wire.available()) (void)Wire.read();
    return false;
  }
  for (size_t i = 0; i < rlen; ++i) r[i] = (uint8_t)Wire.read();
  return true;
}

}  // namespace scratchsor
