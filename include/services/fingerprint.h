#pragma once
#include <cstdint>

namespace scratchsor {
inline constexpr unsigned kFeatureCount = 12, kFeatureFrames = 20;
inline constexpr unsigned kFingerprintCapacity = 128;
// Profiles: male/right, male/left, female/right, female/left.
int reachCategory(uint8_t profile, float referenceMm);
bool referenceAllowed(float referenceMm);
const char* categoryName(int category);
const char* profileName(uint8_t profile);

struct ReachConfig {
  uint8_t profile = 0;
  bool selected = false;
  char setup[96] = "";
};
struct FingerprintPoint {
  float features[kFeatureCount]{};
  float referenceMm = 0;
  uint32_t id = 0, timestampMs = 0;
  uint8_t profile = 0;
  bool test = false;
};
struct FingerprintModel {
  float mean[kFeatureCount]{}, scale[kFeatureCount]{};
  uint16_t confusion[3][3]{};
  uint16_t trainCount[3]{}, testCount[3]{};
  float accuracy = 0, recall[3]{};
  bool ready = false;
};
struct FingerprintData {
  uint32_t signature = 0, nextId = 1;
  float baseline[kFeatureCount]{};
  bool baselineReady = false;
  uint16_t count = 0;
  FingerprintPoint points[kFingerprintCapacity]{};
  FingerprintModel models[4]{};
};
bool appendFingerprint(FingerprintData& data, const FingerprintPoint& point);
bool deleteFingerprint(FingerprintData& data, uint32_t id);
bool fitFingerprint(FingerprintData& data, uint8_t profile);
int predictFingerprint(const FingerprintData& data, uint8_t profile,
                       const float* features, float& vote);
bool validFingerprintData(const FingerprintData& data);

class FeatureWindow {
public:
  void clear();
  bool push(const float* features, float* mean);
private:
  float frames_[kFeatureFrames][kFeatureCount]{};
  double sums_[kFeatureCount]{};
  unsigned count_ = 0, head_ = 0;
};
} // namespace scratchsor
