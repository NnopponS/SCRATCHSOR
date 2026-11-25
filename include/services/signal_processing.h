#pragma once
// Pure C++ signal processing — NO Arduino dependencies so these compile and
// run under `pio test -e native`. All filters are per-axis scalar filters;
// the service layer instantiates one Vec3Filter (three of these) per sensor.

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <array>

namespace scratchsor {

// First-order IIR low-pass: y += alpha*(x - y). alpha in (0,1].
class LowPass {
public:
  explicit LowPass(float alpha = 0.35f) : alpha_(alpha) {}
  void setAlpha(float a) { alpha_ = a; reset(); }
  void reset() { primed_ = false; }
  float update(float x) {
    if (!primed_) { y_ = x; primed_ = true; return y_; }
    y_ += alpha_ * (x - y_);
    return y_;
  }
  float value() const { return y_; }
private:
  float alpha_, y_ = 0;
  bool primed_ = false;
};

// Sliding moving average, fixed capacity N (ring buffer, O(1)).
template <size_t N>
class MovingAverage {
public:
  void reset() { count_ = idx_ = 0; sum_ = 0; }
  float update(float x) {
    if (count_ < N) { sum_ += x; buf_[idx_] = x; idx_ = (idx_ + 1) % N; ++count_; }
    else { sum_ += x - buf_[idx_]; buf_[idx_] = x; idx_ = (idx_ + 1) % N; }
    return count_ ? sum_ / count_ : x;
  }
  float value() const { return count_ ? sum_ / count_ : 0; }
  size_t count() const { return count_; }
private:
  float buf_[N] = {};
  float sum_ = 0;
  size_t count_ = 0, idx_ = 0;
};

// Sliding median, small odd window (insertion into sorted copy, O(W²) fine
// for W<=9). Returns median of the last W samples.
template <size_t W>
class MedianFilter {
public:
  static_assert(W % 2 == 1, "median window must be odd");
  void reset() { count_ = idx_ = 0; }
  float update(float x) {
    buf_[idx_] = x; idx_ = (idx_ + 1) % W;
    if (count_ < W) ++count_;
    std::array<float, W> tmp{};
    for (size_t i = 0; i < count_; ++i) tmp[i] = buf_[i];
    for (size_t i = 1; i < count_; ++i) {           // insertion sort
      float v = tmp[i]; size_t j = i;
      while (j > 0 && tmp[j - 1] > v) { tmp[j] = tmp[j - 1]; --j; }
      tmp[j] = v;
    }
    return tmp[count_ / 2];
  }
private:
  float buf_[W] = {};
  size_t count_ = 0, idx_ = 0;
};

// Running mean/variance (Welford) — noise floor + outlier gating.
class NoiseEstimator {
public:
  void reset() { n_ = 0; mean_ = 0; m2_ = 0; }
  float update(float x) {
    ++n_;
    float d = x - mean_;
    mean_ += d / n_;
    m2_ += d * (x - mean_);
    return stddev();
  }
  float mean() const { return mean_; }
  float stddev() const { return n_ > 1 ? std::sqrt(m2_ / (n_ - 1)) : 0; }
  size_t count() const { return n_; }
  // z-score of candidate sample against current distribution.
  float zscore(float x) const {
    float s = stddev();
    return s > 1e-6f ? std::fabs(x - mean_) / s : 0;
  }
private:
  size_t n_ = 0;
  float mean_ = 0, m2_ = 0;
};

// Per-axis chain assembled by the service from FilterConfig:
// raw -> [median] -> [moving average] -> [low-pass] -> [baseline subtract]
// Outlier gate uses NoiseEstimator.zscore; rejected samples hold last value.
class AxisFilter {
public:
  // Plain-parameter configure keeps this header Arduino-free.
  void configure(bool median, bool avg, bool lp, float lpAlpha,
                 float outlierSigma) {
    useMed_ = median; useAvg_ = avg; useLp_ = lp;
    lp_.setAlpha(lpAlpha); outlierSigma_ = outlierSigma;
    reset();
  }
  void reset();
  void setBaseline(float b) { baseline_ = b; haveBaseline_ = true; }
  void clearBaseline() { haveBaseline_ = false; baseline_ = 0; }
  bool hasBaseline() const { return haveBaseline_; }
  float baseline() const { return baseline_; }

  // Returns filtered value; sets isOutlier when the gate rejected x.
  float update(float x, bool& isOutlier);
  float quality() const;      // 0..1 heuristic from noise level
  float noiseStddev() const { return noise_.stddev(); }
private:
  bool useMed_ = false, useAvg_ = true, useLp_ = true;
  float outlierSigma_ = 4.0f;
  MovingAverage<8> avg_;
  MedianFilter<9> med_;
  LowPass lp_;
  NoiseEstimator noise_;
  float baseline_ = 0;
  float last_ = 0;
  bool haveBaseline_ = false;
  bool primed_ = false;
  uint8_t rejected_ = 0;
};

// One filter chain for a full vector.
class Vec3Filter {
public:
  void configure(bool median, bool avg, bool lp, float lpAlpha,
                 float outlierSigma) {
    fx_.configure(median, avg, lp, lpAlpha, outlierSigma);
    fy_.configure(median, avg, lp, lpAlpha, outlierSigma);
    fz_.configure(median, avg, lp, lpAlpha, outlierSigma);
  }
  void reset() { fx_.reset(); fy_.reset(); fz_.reset(); }
  void setBaseline(float bx, float by, float bz) {
    fx_.setBaseline(bx); fy_.setBaseline(by); fz_.setBaseline(bz);
  }
  void clearBaseline() { fx_.clearBaseline(); fy_.clearBaseline(); fz_.clearBaseline(); }
  AxisFilter& x() { return fx_; }
  AxisFilter& y() { return fy_; }
  AxisFilter& z() { return fz_; }
private:
  AxisFilter fx_, fy_, fz_;
};

}  // namespace scratchsor
