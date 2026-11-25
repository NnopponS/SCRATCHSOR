// SCRATCHSOR — calibration mathematics (ordinary least squares).
// Pure C++ (no Arduino headers): compiled into firmware and directly
// #included by test/test_calibration_math. See calibration_math.h for the
// dipole-linearization model this implements.

#include "services/calibration_math.h"

namespace scratchsor {
namespace calmath {
namespace {

// Degenerate-guard constants for the normal-equation solver.
constexpr float kMinXSpan   = 1e-9f;   // x must span a non-zero range
constexpr float kPivotRelEps = 1e-9f;  // pivot < eps*maxCoeff => singular

// Solve A*c = b for a dense n x n matrix A (row-major) via Gaussian
// elimination with partial pivoting. Works on copies supplied by the caller.
// Returns false when a pivot is effectively singular relative to the matrix.
bool solveSquare(float* a, float* b, float* c, int n) {
  float maxAbs = 0;
  for (int i = 0; i < n * n; ++i) {
    const float v = std::fabs(a[i]);
    if (v > maxAbs) maxAbs = v;
  }
  if (!(maxAbs > 0)) return false;   // all-zero matrix (also catches NaN)
  const float pivotFloor = kPivotRelEps * maxAbs;

  for (int col = 0; col < n; ++col) {
    int piv = col;
    for (int r = col + 1; r < n; ++r) {
      if (std::fabs(a[r * n + col]) > std::fabs(a[piv * n + col])) piv = r;
    }
    if (std::fabs(a[piv * n + col]) < pivotFloor) return false;
    if (piv != col) {
      for (int k = col; k < n; ++k) {
        const float t = a[col * n + k];
        a[col * n + k] = a[piv * n + k];
        a[piv * n + k] = t;
      }
      const float tb = b[col]; b[col] = b[piv]; b[piv] = tb;
    }
    for (int r = col + 1; r < n; ++r) {
      const float f = a[r * n + col] / a[col * n + col];
      for (int k = col; k < n; ++k) a[r * n + k] -= f * a[col * n + k];
      b[r] -= f * b[col];
    }
  }
  for (int r = n - 1; r >= 0; --r) {
    float s = b[r];
    for (int k = r + 1; k < n; ++k) s -= a[r * n + k] * c[k];
    c[r] = s / a[r * n + r];
  }
  return true;
}

}  // namespace

FitResult fitPoly(const float* x, const float* y, size_t n, bool quadratic) {
  FitResult r;
  const size_t need = quadratic ? 3 : 2;
  if (!x || !y || n < need) return r;

  // x must span a non-zero range (NaN-safe: any NaN makes this check fail).
  float xmin = x[0], xmax = x[0];
  for (size_t i = 1; i < n; ++i) {
    if (x[i] < xmin) xmin = x[i];
    if (x[i] > xmax) xmax = x[i];
  }
  if (!(xmax - xmin > kMinXSpan)) return r;

  // Normal equations for d(x) = c0 + c1*x (+ c2*x^2):
  //   A[j][k] = sum_i x_i^(j+k)   b[j] = sum_i x_i^j * y_i
  const int p = quadratic ? 3 : 2;
  float a[9] = {0}, b[3] = {0}, c[3] = {0};
  for (size_t i = 0; i < n; ++i) {
    float xp[5];
    xp[0] = 1.0f;
    for (int k = 1; k < 5; ++k) xp[k] = xp[k - 1] * x[i];
    for (int j = 0; j < p; ++j) {
      b[j] += xp[j] * y[i];
      for (int k = 0; k < p; ++k) a[j * p + k] += xp[j + k];
    }
  }
  if (!solveSquare(a, b, c, p)) return r;

  for (int j = 0; j < p; ++j) r.coeffs[j] = c[j];
  float sae = 0;
  for (size_t i = 0; i < n; ++i) {
    float d = c[0] + c[1] * x[i];
    if (quadratic) d += c[2] * x[i] * x[i];
    sae += std::fabs(d - y[i]);
  }
  r.maeTrain = sae / (float)n;
  r.ok = true;
  return r;
}

Metrics evaluate(const float* coeffs, bool quadratic,
                 const float* x, const float* y, size_t n) {
  Metrics m;
  m.n = n;
  if (!coeffs || !x || !y || n == 0) return m;
  float sae = 0, sse = 0, mx = 0;
  for (size_t i = 0; i < n; ++i) {
    float d = coeffs[0] + coeffs[1] * x[i];
    if (quadratic) d += coeffs[2] * x[i] * x[i];
    const float e = std::fabs(d - y[i]);
    sae += e;
    sse += e * e;
    if (e > mx) mx = e;
  }
  m.mae = sae / (float)n;
  m.rmse = std::sqrt(sse / (float)n);
  m.maxErr = mx;
  return m;
}

void trainValidSplit(size_t n, size_t everyK,
                     size_t* trainIdx, size_t& nTrain,
                     size_t* validIdx, size_t& nValid) {
  nTrain = 0;
  nValid = 0;
  if (!trainIdx || !validIdx || n == 0) return;
  if (everyK < 2) everyK = 2;   // k=1 would starve the train set

  if (n >= everyK * 2) {
    // Standard case: every k-th sample (1-indexed) goes to validation.
    for (size_t i = 0; i < n; ++i) {
      if ((i + 1) % everyK == 0) validIdx[nValid++] = i;
      else                       trainIdx[nTrain++] = i;
    }
  } else {
    // Small-n case: hold out ~20% (minimum 1) evenly spaced across the set.
    size_t nV = n / 5;
    if (nV < 1) nV = 1;
    const size_t stride = n / nV;          // >= 1; picks are strictly increasing
    for (size_t k = 1; k <= nV; ++k) {
      size_t idx = k * stride - 1;
      if (idx >= n) idx = n - 1;
      validIdx[nValid++] = idx;
    }
    size_t vi = 0;
    for (size_t i = 0; i < n; ++i) {
      if (vi < nValid && validIdx[vi] == i) ++vi;   // held out
      else trainIdx[nTrain++] = i;
    }
  }
}

}  // namespace calmath
}  // namespace scratchsor
