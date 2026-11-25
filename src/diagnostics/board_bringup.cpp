// Explicit diagnostic environment only; excluded from normal application.
// Repeats output so a late serial connection still reports boot and bus health.
#ifdef SCRATCHSOR_BOARD_BRINGUP
#include <Arduino.h>
#include <Wire.h>
#include "pins.h"
#include "config.h"

namespace {
bool ack(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool select(uint8_t mux, uint8_t mask) {
  Wire.beginTransmission(mux);
  Wire.write(mask);
  return Wire.endTransmission() == 0;
}

void scan() {
  uint8_t mux = 0;
  for (uint8_t a = 0x70; a <= 0x77; ++a) {
    if (ack(a)) { mux = a; break; }
  }
  if (!mux) {
    Serial.println("[BRINGUP] No mux at 0x70..0x77 (expected on bare board)");
    return;
  }
  Serial.println("[BRINGUP] Expected OLED CH0; sensor 1..4 CH2..5");
  Serial.printf("[BRINGUP] Mux ACK at 0x%02X\n", mux);
  for (uint8_t ch = 0; ch < 8; ++ch) {
    if (!select(mux, uint8_t(1u << ch))) {
      Serial.printf("[BRINGUP] CH%u select failed\n", ch);
      continue;
    }
    Serial.printf("[BRINGUP] CH%u:", ch);
    unsigned count = 0;
    for (uint8_t a = 0x03; a <= 0x77; ++a) {
      if (a != mux && ack(a)) {
        Serial.printf(" 0x%02X", a);
        ++count;
      }
    }
    Serial.println(count ? "" : " empty");
  }
  select(mux, 0);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[BRINGUP] setup reached; standalone diagnostic");
  Wire.begin(scratchsor::kPinI2cSda, scratchsor::kPinI2cScl, 100000);
  Wire.setTimeOut(25);
}

void loop() {
  Serial.printf("[BRINGUP] alive uptime=%lu ms heap=%u SDA=%d SCL=%d\n",
                (unsigned long)millis(), ESP.getFreeHeap(),
                digitalRead(scratchsor::kPinI2cSda),
                digitalRead(scratchsor::kPinI2cScl));
  scan();
  Serial.flush();
  delay(3000);
}
#endif
