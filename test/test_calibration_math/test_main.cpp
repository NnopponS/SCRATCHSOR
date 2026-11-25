// Native unit tests for calibration mathematics.
// env:native does NOT compile src/, so the implementation is #included
// directly (it is Arduino-free by contract).

#include <unity.h>
#include <cmath>
#include "services/calibration_math.h"
#include "../../src/services/calibration_math.cpp"

using namespace scratchsor::calmath;

void setUp(void) {}
void tearDown(void) {}

// ------------------------------- fitPoly ---------------------------------

static void test_fit_recovers_known_line(void) {
  const float x[] = {0, 1, 2, 3, 4};
  const float y[] = {1, 3, 5, 7, 9};            // y = 2x + 1
  const FitResult r = fitPoly(x, y, 5, false);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, r.coeffs[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, r.coeffs[1]);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, r.maeTrain);
}

static void test_fit_recovers_quadratic(void) {
  const float x[] = {0, 1, 2, 3, 4};
  const float y[] = {0, 1, 4, 9, 16};           // y = x^2
  const FitResult r = fitPoly(x, y, 5, true);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, r.coeffs[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, r.coeffs[1]);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, r.coeffs[2]);
}

static void test_fit_degenerate_inputs(void) {
  const float flatX[] = {1, 1, 1};
  const float flatY[] = {2, 2, 2};
  // Zero x-range -> no slope can be resolved.
  TEST_ASSERT_FALSE(fitPoly(flatX, flatY, 3, false).ok);
  // n below minimum for each model order.
  TEST_ASSERT_FALSE(fitPoly(flatX, flatY, 1, false).ok);
  TEST_ASSERT_FALSE(fitPoly(flatX, flatY, 2, true).ok);
  // Null pointers must fail, not crash.
  TEST_ASSERT_FALSE(fitPoly(nullptr, flatY, 3, false).ok);
}

// ---------------------------- dipoleFeature ------------------------------

static void test_dipole_feature_monotonic_and_domain(void) {
  TEST_ASSERT_TRUE(std::isnan(dipoleFeature(0.0f)));
  TEST_ASSERT_TRUE(std::isnan(dipoleFeature(-5.0f)));
  // (1/B)^(1/3) strictly decreasing in B.
  TEST_ASSERT_TRUE(dipoleFeature(1.0f) > dipoleFeature(8.0f));
  TEST_ASSERT_TRUE(dipoleFeature(8.0f) > dipoleFeature(27.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, dipoleFeature(8.0f));
}

// ------------------------------- evaluate --------------------------------

static void test_evaluate_metrics_on_known_residuals(void) {
  const float c[4] = {1.0f, 2.0f, 0, 0};        // d = 1 + 2x
  const float x[] = {0.0f, 1.0f};
  const float y[] = {1.5f, 3.5f};               // errors 0.5, 0.5
  const Metrics m = evaluate(c, false, x, y, 2);
  TEST_ASSERT_EQUAL_UINT(2u, (unsigned)m.n);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, m.mae);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, m.rmse);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, m.maxErr);
}

static void test_evaluate_empty_set(void) {
  const float c[4] = {0, 1, 0, 0};
  const Metrics m = evaluate(c, false, nullptr, nullptr, 0);
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)m.n);
  TEST_ASSERT_FLOAT_WITHIN(1e-9f, 0.0f, m.rmse);
}

// ---------------------------- trainValidSplit ----------------------------

static void test_split_every_kth(void) {
  size_t tr[16], va[16], nT = 0, nV = 0;
  trainValidSplit(10, 5, tr, nT, va, nV);
  TEST_ASSERT_EQUAL_UINT(2u, (unsigned)nV);     // indices 4 and 9 held out
  TEST_ASSERT_EQUAL_UINT(8u, (unsigned)nT);
  TEST_ASSERT_EQUAL_UINT(4u, (unsigned)va[0]);
  TEST_ASSERT_EQUAL_UINT(9u, (unsigned)va[1]);
}

static void test_split_small_n_minimum_one(void) {
  size_t tr[16], va[16], nT = 0, nV = 0;
  trainValidSplit(4, 5, tr, nT, va, nV);        // 4 < 2*5 -> ~20% branch
  TEST_ASSERT_EQUAL_UINT(1u, (unsigned)nV);
  TEST_ASSERT_EQUAL_UINT(3u, (unsigned)nT);
  // Exact partition: every index in exactly one set.
  bool seen[4] = {false, false, false, false};
  for (size_t i = 0; i < nT; ++i) seen[tr[i]] = true;
  for (size_t i = 0; i < nV; ++i) {
    TEST_ASSERT_FALSE(seen[va[i]]);
    seen[va[i]] = true;
  }
  for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(seen[i]);
}

static void test_split_medium_n(void) {
  size_t tr[16], va[16], nT = 0, nV = 0;
  trainValidSplit(6, 5, tr, nT, va, nV);        // 6 < 10 -> 1 held out
  TEST_ASSERT_EQUAL_UINT(1u, (unsigned)nV);
  TEST_ASSERT_EQUAL_UINT(5u, (unsigned)nT);
}

// ------------------------------ end-to-end -------------------------------

// Fit a dipole-like dataset: d = 10 + 40*u where u = (1/B)^(1/3). Feeding
// dipoleFeature(B) must reproduce u exactly, so the fit recovers c0/c1.
static void test_fit_recovers_dipole_model(void) {
  float x[8], y[8];
  for (int i = 0; i < 8; ++i) {
    const float u = 0.25f + 0.1f * (float)i;    // feature values 0.25..0.95
    const float b = 1.0f / (u * u * u);         // B = 1/u^3
    x[i] = dipoleFeature(b);
    y[i] = 10.0f + 40.0f * u;
  }
  const FitResult r = fitPoly(x, y, 8, false);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FLOAT_WITHIN(1e-2f, 10.0f, r.coeffs[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-2f, 40.0f, r.coeffs[1]);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, r.maeTrain);
}

// --------------------------------- main ----------------------------------

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_fit_recovers_known_line);
  RUN_TEST(test_fit_recovers_quadratic);
  RUN_TEST(test_fit_degenerate_inputs);
  RUN_TEST(test_dipole_feature_monotonic_and_domain);
  RUN_TEST(test_evaluate_metrics_on_known_residuals);
  RUN_TEST(test_evaluate_empty_set);
  RUN_TEST(test_split_every_kth);
  RUN_TEST(test_split_small_n_minimum_one);
  RUN_TEST(test_split_medium_n);
  RUN_TEST(test_fit_recovers_dipole_model);
  return UNITY_END();
}
