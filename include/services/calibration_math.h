#pragma once
// Pure C++ calibration mathematics — Arduino-free, native-testable.
//
// Physical basis: a small permanent magnet approximates a dipole; axial
// field falls as 1/r^3. With baseline-subtracted magnitude B (uT), the
// feature u = (1/B)^(1/3) is approximately linear in distance. Model types:
//   DIPOLE_LINEAR: d = c0 + c1*u
//   DIPOLE_QUAD:   d = c0 + c1*u + c2*u^2
// Fitting = ordinary least squares on the train set; validation metrics are
// computed on the held-out set ONLY (never overlapping recordings).
//
// This is an experimental pipeline — a "validated" model is a statement
// about the collected dataset, not a universal accuracy guarantee.

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace scratchsor {
namespace calmath {

struct FitResult {
  bool ok = false;
  float coeffs[4] = {0};   // c0..c2 (+spare)
  float maeTrain = 0;
};

// Least-squares fit of y ≈ c0 + c1*x (+ c2*x^2 when quadratic).
// n must be >= (quadratic ? 3 : 2) and x must span a non-zero range.
FitResult fitPoly(const float* x, const float* y, size_t n, bool quadratic);

// Dipole-linearized feature: (1/B_uT)^(1/3). B<=0 -> NaN (caller skips).
inline float dipoleFeature(float bUt) {
  return bUt > 0 ? std::pow(1.0f / bUt, 1.0f / 3.0f) : NAN;
}

struct Metrics {
  float mae = 0, rmse = 0, maxErr = 0;
  size_t n = 0;
};

// Evaluate model d(x) = c0 + c1*x + c2*x^2 on held-out (x,y) pairs.
Metrics evaluate(const float* coeffs, bool quadratic,
                 const float* x, const float* y, size_t n);

// Simple deterministic split: every kth sample goes to validation.
// Returns counts via out params; caller provides index buffers.
void trainValidSplit(size_t n, size_t everyK,
                     size_t* trainIdx, size_t& nTrain,
                     size_t* validIdx, size_t& nValid);

}  // namespace calmath
}  // namespace scratchsor
