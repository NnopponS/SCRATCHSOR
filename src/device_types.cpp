// SCRATCHSOR — shared vocabulary types: enum strings + Vec3 helpers.
// Keep the short strings in sync with the enums in device_types.h; they are
// used verbatim on the wire ("st" field, see protocol.h) and on the OLED.

#include "device_types.h"
#include <cmath>

namespace scratchsor {

float Vec3::magnitude() const {
  return std::sqrt(x * x + y * y + z * z);
}

const char* toString(SensorStatus s) {
  switch (s) {
    case SensorStatus::OK:         return "OK";
    case SensorStatus::NOT_FOUND:  return "NOT_FOUND";
    case SensorStatus::NACK:       return "NACK";
    case SensorStatus::TIMEOUT:    return "TIMEOUT";
    case SensorStatus::SATURATED:  return "SATURATED";
    case SensorStatus::COMM_ERROR: return "COMM_ERROR";
    case SensorStatus::DISABLED:   return "DISABLED";
    case SensorStatus::UNKNOWN:
    default:                       return "UNKNOWN";
  }
}

const char* toString(WifiStaState s) {
  switch (s) {
    case WifiStaState::CONNECTING: return "CONNECTING";
    case WifiStaState::CONNECTED:  return "CONNECTED";
    case WifiStaState::FAILED:     return "FAILED";
    case WifiStaState::DISABLED:   return "DISABLED";
    case WifiStaState::IDLE:
    default:                       return "IDLE";
  }
}

const char* toString(MeasurementState s) {
  switch (s) {
    case MeasurementState::CALIBRATING:    return "CALIBRATING";
    case MeasurementState::VALID:          return "VALID";
    case MeasurementState::LOW_CONFIDENCE: return "LOW_CONFIDENCE";
    case MeasurementState::OUT_OF_RANGE:   return "OUT_OF_RANGE";
    case MeasurementState::SENSOR_ERROR:   return "SENSOR_ERROR";
    case MeasurementState::WARMING_UP:     return "WARMING_UP";
    case MeasurementState::NOT_CALIBRATED:
    default:                               return "NOT_CALIBRATED";
  }
}

}  // namespace scratchsor
