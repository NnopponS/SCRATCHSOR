// SCRATCHSOR — diagnostics: health aggregation, boot report, JSON export.
// toString() implementations live in device_types.cpp, NOT here.

#include "diagnostics/diagnostics.h"
// U8g2lib.h MUST precede oled_manager.h: the frozen header holds
// std::unique_ptr<U8G2> with only a forward declaration, so U8G2 must be
// complete in every TU that includes it.
#include <U8g2lib.h>
#include "hardware/mux_driver.h"
#include "hardware/oled_manager.h"
#include "services/sensor_service.h"
#include <cstdarg>
#include <cstdio>

namespace scratchsor {
namespace {

const char* controllerName(OledController c) {
  switch (c) {
    case OledController::SSD1306: return "SSD1306";
    case OledController::SH1106:  return "SH1106";
    default:                      return "UNKNOWN";
  }
}

// Bounded append helper for healthToJson — keeps buf NUL-terminated even
// when the output truncates (caller sees the truncated length returned).
void jcat(char* buf, size_t len, size_t& off, const char* fmt, ...) {
  if (off >= len) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf + off, len - off, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  off += (size_t)n;
  if (off >= len) off = len - 1;
}

}  // namespace

void buildHealthReport(HealthReport& out, const MuxDriver& mux,
                       const SensorService& sensors, const OledManager& oled) {
  out = HealthReport{};
  out.uptimeMs = millis();
  out.freeHeap = ESP.getFreeHeap();
  out.muxFound = mux.present();
  out.muxAddr = mux.addr();
  out.oledFound = oled.present();
  out.oledAddr = oled.addr();
  out.oledController = oled.controller();
  sensors.health(out);   // fills sensor[] counters + measuredRateHz
}

void printBootReport(const HealthReport& h) {
  SCR_LOGI("---- boot health ------------------------------------");
  SCR_LOGI(" uptime   %lu ms   free heap %lu B",
           (unsigned long)h.uptimeMs, (unsigned long)h.freeHeap);
  if (h.muxFound) {
    SCR_LOGI(" mux      found @ 0x%02X", (unsigned)h.muxAddr);
  } else {
    SCR_LOGW(" mux      NOT FOUND — sensor/OLED channels unreachable");
  }
  if (h.oledFound) {
    SCR_LOGI(" oled     found @ 0x%02X (%s)", (unsigned)h.oledAddr,
             controllerName(h.oledController));
  } else {
    SCR_LOGW(" oled     NOT FOUND — running headless");
  }
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    const SensorHealth& s = h.sensor[i];
    if (s.status == SensorStatus::OK) {
      SCR_LOGI(" sensor%u  0x%02X %-9s ok=%lu err=%lu", (unsigned)i,
               (unsigned)s.i2cAddr, toString(s.status),
               (unsigned long)s.okCount, (unsigned long)s.errCount);
    } else {
      SCR_LOGW(" sensor%u  %-9s ok=%lu err=%lu", (unsigned)i,
               toString(s.status), (unsigned long)s.okCount,
               (unsigned long)s.errCount);
    }
  }
  SCR_LOGI(" rate     %.2f Hz measured (target ~%.1f Hz)",
           (double)h.measuredRateHz, (double)kSampleRateHz);
  SCR_LOGI("------------------------------------------------------");
}

// Shape fixed by docs/api.md GET /api/v1/health:
//   {"mux":true,"muxAddr":112,"oled":true,"oledAddr":60,
//    "controller":"SSD1306","rate":9.8,
//    "sensors":[{"id":0,"st":"OK","ok":12,"err":0},...]}
size_t healthToJson(const HealthReport& h, char* buf, size_t len) {
  if (!buf || len == 0) return 0;
  buf[0] = 0;
  size_t off = 0;
  jcat(buf, len, off,
       "{\"mux\":%s,\"muxAddr\":%u,\"oled\":%s,\"oledAddr\":%u,"
       "\"controller\":\"%s\",\"rate\":%.2f,\"sensors\":[",
       h.muxFound ? "true" : "false", (unsigned)h.muxAddr,
       h.oledFound ? "true" : "false", (unsigned)h.oledAddr,
       controllerName(h.oledController), (double)h.measuredRateHz);
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    jcat(buf, len, off, "%s{\"id\":%u,\"st\":\"%s\",\"ok\":%lu,\"err\":%lu}",
         i ? "," : "", (unsigned)i, toString(h.sensor[i].status),
         (unsigned long)h.sensor[i].okCount,
         (unsigned long)h.sensor[i].errCount);
  }
  jcat(buf, len, off, "]}");
  buf[off] = 0;
  return off;
}

}  // namespace scratchsor
