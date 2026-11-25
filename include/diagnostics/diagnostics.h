#pragma once
// Diagnostics: serial boot report, health aggregation, log helpers.
// Log macros honor CORE_DEBUG_LEVEL and never print secrets (STA password,
// AP password outside the dedicated OLED setup screen).

#include <Arduino.h>
#include "device_types.h"

#define SCR_LOGI(fmt, ...) Serial.printf("[I] " fmt "\n", ##__VA_ARGS__)
#define SCR_LOGW(fmt, ...) Serial.printf("[W] " fmt "\n", ##__VA_ARGS__)
#define SCR_LOGE(fmt, ...) Serial.printf("[E] " fmt "\n", ##__VA_ARGS__)
#ifdef SCRATCHSOR_DEBUG
#define SCR_LOGD(fmt, ...) Serial.printf("[D] " fmt "\n", ##__VA_ARGS__)
#else
#define SCR_LOGD(fmt, ...) do {} while (0)
#endif

namespace scratchsor {

class MuxDriver;
class SensorService;
class OledManager;

// Assemble a full HealthReport from live subsystems.
void buildHealthReport(HealthReport& out, const MuxDriver& mux,
                       const SensorService& sensors, const OledManager& oled);

// Pretty-print boot health table to Serial.
void printBootReport(const HealthReport& h);

// Serialize health report to JSON (into caller buffer). Returns length.
size_t healthToJson(const HealthReport& h, char* buf, size_t len);

}  // namespace scratchsor
