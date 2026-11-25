#pragma once
#include <cstdint>
#include <cmath>

namespace scratchsor {
// A calibrated one-dimensional fixture, not unconstrained magnetic localization.
struct GapConfig {
  bool enabled = true;
  bool fixedGeometry = false;
  uint8_t sensorId = 0;
  uint8_t reference = 0; // 0: facing surfaces, 1: magnet centers
  char setup[96] = ""; // orientation, mounting and magnet identity
};

inline uint32_t gapSignature(const GapConfig& g) {
  uint32_t h = 2166136261u;
  auto add = [&h](uint8_t b) { h = (h ^ b) * 16777619u; };
  add(g.enabled); add(g.fixedGeometry); add(g.sensorId); add(g.reference);
  for (unsigned i = 0; i < sizeof(g.setup) && g.setup[i]; ++i) add(g.setup[i]);
  return h;
}

inline bool gapReady(const GapConfig& g) {
  return !g.enabled || (g.fixedGeometry && g.sensorId < 4 && g.reference < 2);
}

inline bool gapModelUsable(const GapConfig& g, uint32_t signature,
                           uint8_t modelMask, uint8_t liveMask) {
  return gapReady(g) && signature == gapSignature(g) && modelMask != 0 &&
         liveMask == modelMask;
}

inline bool gapReferenceAllowed(float mm) {
  return std::isfinite(mm) && mm > 0.0f && mm <= 500.0f;
}

inline bool gapValidationPass(unsigned heldOut, float rmse, float maxError) {
  return heldOut >= 2 && std::isfinite(rmse) && std::isfinite(maxError) &&
         rmse >= 0 && maxError >= 0 && rmse <= 5.0f && maxError <= 15.0f;
}
} // namespace scratchsor
