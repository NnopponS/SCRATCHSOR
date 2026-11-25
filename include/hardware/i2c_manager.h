#pragma once
// Centralized, mutex-guarded owner of the primary I2C bus (Wire).
// EVERY transaction to a downstream device must:
//   1. Acquire the bus (RAII guard below, or lock()/unlock()).
//   2. Select the correct PCA9548A channel (never assume it persisted).
//   3. Perform transfers with bounded timeouts.
//
// Usage pattern:
//   {
//     I2CBusGuard guard(i2c, 3);            // locks + selects channel 3
//     if (!guard.ok()) return ...;          // lock or channel-select failed
//     guard.write(...); guard.read(...);
//   }                                       // unlocks automatically

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <functional>
#include "config.h"

namespace scratchsor {

class I2CManager {
public:
  I2CManager() = default;

  // Initialize Wire on pins.h SDA/SCL at kI2cClockHz. Idempotent.
  bool begin();

  // Raw lock/unlock — prefer I2CBusGuard. Returns false on timeout.
  bool lock(uint32_t timeoutMs = kI2cLockTimeoutMs);
  void unlock();

  // Select mux channel 0..7. Must hold the lock. Writes the channel bitmask
  // to the mux at the (already resolved) address. Returns false on NACK.
  bool selectChannel(uint8_t channel);

  // Deselect all mux channels. Must hold the lock.
  bool deselectAll();

  // Resolve mux address by scanning 0x70..0x77 once. Returns found addr or 0.
  // Acquires/releases the lock internally. Call before any selectChannel.
  uint8_t detectMux();

  // Probe a single 7-bit address. Must hold the lock (and have the channel
  // selected for downstream devices). Returns true on ACK.
  bool probe(uint8_t addr);

  // Convenience: run fn(guardless) with lock held + channel selected.
  // fn signature: bool() — return value propagates. Lock is always released.
  bool withChannel(uint8_t channel, const std::function<bool()>& fn,
                   uint32_t timeoutMs = kI2cLockTimeoutMs);

  // Low-level transfers, lock + channel already held by caller.
  bool write(uint8_t addr, const uint8_t* data, size_t len, bool sendStop = true);
  bool writeByte(uint8_t addr, uint8_t value);
  bool read(uint8_t addr, uint8_t* buf, size_t len);
  // Write then repeated-start read.
  bool writeRead(uint8_t addr, const uint8_t* w, size_t wlen,
                 uint8_t* r, size_t rlen);

  uint8_t muxAddr() const { return muxAddr_; }
  bool    muxFound() const { return muxFound_; }
  uint8_t currentChannel() const { return currentChannel_; }  // 0xFF = none

private:
  SemaphoreHandle_t mutex_ = nullptr;
  uint8_t  muxAddr_ = 0;
  bool     muxFound_ = false;
  uint8_t  currentChannel_ = 0xFF;
  bool     begun_ = false;
};

// RAII guard: locks the bus and selects a channel for its lifetime.
class I2CBusGuard {
public:
  I2CBusGuard(I2CManager& mgr, uint8_t channel,
              uint32_t timeoutMs = kI2cLockTimeoutMs)
      : mgr_(mgr), locked_(mgr.lock(timeoutMs)),
        ok_(locked_ && mgr.selectChannel(channel)) {}
  ~I2CBusGuard() { if (locked_) mgr_.unlock(); }

  // lock-only guard (no channel select) — used for mux detection itself.
  explicit I2CBusGuard(I2CManager& mgr, uint32_t timeoutMs)
      : mgr_(mgr), locked_(mgr.lock(timeoutMs)), ok_(locked_) {}

  I2CBusGuard(const I2CBusGuard&) = delete;
  I2CBusGuard& operator=(const I2CBusGuard&) = delete;

  bool ok() const { return ok_; }

private:
  I2CManager& mgr_;
  bool locked_ = false;   // declared before ok_: init order is mgr_->locked_->ok_
  bool ok_ = false;
};

// Global bus instance (defined in i2c_manager.cpp, constructed in main).
extern I2CManager gI2c;

}  // namespace scratchsor
