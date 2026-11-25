// SCRATCHSOR — per-axis scalar signal processing.
// Pure C++ (no Arduino headers): this file is compiled into the firmware AND
// directly #included by the native unit tests (test/test_signal_processing).
// Keep it dependency-free: only the matching header + <cmath>.

#include "services/signal_processing.h"

namespace scratchsor {

void AxisFilter::reset() {
  // Runtime state only. The use*/outlierSigma_ config fields are intentionally
  // left alone — configure() sets them and then calls reset().
  avg_.reset();
  med_.reset();
  lp_.reset();
  noise_.reset();
  baseline_ = 0;
  last_ = 0;
  haveBaseline_ = false;
  primed_ = false;
  rejected_ = 0;
}

float AxisFilter::update(float x, bool& isOutlier) {
  isOutlier = false;
  // Outlier gate: armed only once the noise estimator has >= 10 samples so
  // its stddev is meaningful, and disabled entirely when outlierSigma_ <= 0.
  // Isolated spikes hold the last value; sustained changes re-arm the gate.
  if (outlierSigma_ > 0.0f && noise_.count() >= 10 &&
      noise_.zscore(x) > outlierSigma_) {
    if (++rejected_ < 10) {
      isOutlier = true;
      return last_;
    }
    noise_.reset();
  }
  rejected_ = 0;
  noise_.update(x);
  // Chain: raw -> [median] -> [moving average] -> [low-pass] -> [- baseline]
  float v = x;
  if (useMed_) v = med_.update(v);
  if (useAvg_) v = avg_.update(v);
  if (useLp_)  v = lp_.update(v);
  last_ = haveBaseline_ ? v - baseline_ : v;
  primed_ = true;
  return last_;
}

float AxisFilter::quality() const {
  // Monotone heuristic in (0,1]: noise stddev <= 0.5 uT -> 1.0, above that it
  // decays as 0.5/sd (~0.1 at 5 uT, ~0.01 at 50 uT, -> 0 as sd grows).
  const float sd = noise_.stddev();
  return sd <= 0.5f ? 1.0f : 0.5f / sd;
}

}  // namespace scratchsor
