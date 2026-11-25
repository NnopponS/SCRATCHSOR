// Native unit tests for the pure signal-processing chain.
// env:native does NOT compile src/, so the implementation is #included
// directly (it is Arduino-free by contract).

#include <unity.h>
#include <cmath>
#include "services/signal_processing.h"
#include "../../src/services/signal_processing.cpp"

using namespace scratchsor;

void setUp(void) {}
void tearDown(void) {}

// ------------------------------- LowPass --------------------------------

static void test_lowpass_converges_to_step(void) {
  LowPass lp(0.5f);
  lp.update(0.0f);                         // prime at 0
  float v = 0;
  for (int i = 0; i < 40; ++i) v = lp.update(10.0f);  // step to 10
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, v);
}

static void test_lowpass_first_sample_primes(void) {
  LowPass lp(0.3f);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 42.0f, lp.update(42.0f));
}

// ---------------------------- MovingAverage ------------------------------

static void test_moving_average_window_accuracy(void) {
  MovingAverage<4> ma;
  ma.update(1); ma.update(2); ma.update(3); ma.update(4);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.5f, ma.value());
  // Slides: drops the 1, window becomes {2,3,4,5}.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.5f, ma.update(5));
}

// ----------------------------- MedianFilter ------------------------------

static void test_median_rejects_single_spike(void) {
  MedianFilter<5> med;
  med.update(10); med.update(10); med.update(10); med.update(10);
  // Window {10,10,10,10,100} -> median 10: one spike can't move it.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 10.0f, med.update(100.0f));
}

// ---------------------------- NoiseEstimator -----------------------------

static void test_noise_estimator_mean_stddev(void) {
  NoiseEstimator ne;
  const float data[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  for (float v : data) ne.update(v);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f, ne.mean());
  // Sample stddev of 1..5 = sqrt(10/4) = sqrt(2.5).
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, std::sqrt(2.5f), ne.stddev());
}

// ------------------------------ AxisFilter -------------------------------

static void test_axis_outlier_gate_holds_last(void) {
  AxisFilter f;
  f.configure(false, false, false, 1.0f, 4.0f);  // pass-through, 4-sigma gate
  bool o = false;
  float last = 0;
  // 25 alternating samples establish noise: mean ~10, sd ~0.105.
  for (int i = 0; i < 25; ++i) last = f.update(i % 2 ? 10.1f : 9.9f, o);
  TEST_ASSERT_FALSE(o);
  const float v = f.update(100.0f, o);   // ~850 sigma -> rejected
  TEST_ASSERT_TRUE(o);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, last, v);   // held previous value (~10)
}

static void test_axis_baseline_subtraction(void) {
  AxisFilter f;
  f.configure(false, false, false, 1.0f, 0.0f);  // pass-through, gate off
  bool o = false;
  f.update(10.0f, o);
  f.setBaseline(4.0f);
  TEST_ASSERT_TRUE(f.hasBaseline());
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.0f, f.update(10.0f, o));
  f.clearBaseline();
  TEST_ASSERT_FALSE(f.hasBaseline());
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 10.0f, f.update(10.0f, o));
}

static void test_axis_tracks_sustained_field_change_after_baseline(void) {
  AxisFilter f;
  f.configure(false, true, true, 0.35f, 4.0f);
  bool outlier = false;
  for (int i = 0; i < 25; ++i) f.update(i % 2 ? 10.1f : 9.9f, outlier);
  f.setBaseline(10.0f);
  float value = 0;
  for (int i = 0; i < 100; ++i) value = f.update(100.0f, outlier);
  TEST_ASSERT_FALSE(outlier);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 90.0f, value);
}

static void test_axis_median_blocks_spike_in_chain(void) {
  AxisFilter f;
  f.configure(true, false, false, 1.0f, 0.0f);   // median only, gate off
  bool o = false;
  for (int i = 0; i < 9; ++i) f.update(10.0f, o);
  // Median window (9) holds eight 10s + one 500 -> output stays 10.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 10.0f, f.update(500.0f, o));
}

static void test_axis_moving_average_in_chain(void) {
  AxisFilter f;
  f.configure(false, true, false, 1.0f, 0.0f);   // moving avg only
  bool o = false;
  float v = 0;
  for (int i = 1; i <= 8; ++i) v = f.update((float)i, o);
  // Window 8 holds 1..8 -> mean 4.5.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 4.5f, v);
}

static void test_axis_quality_bounds(void) {
  AxisFilter f;
  f.configure(false, true, true, 0.5f, 0.0f);
  bool o = false;
  for (int i = 0; i < 30; ++i) f.update(10.0f, o);   // noiseless -> q ~ 1
  TEST_ASSERT_TRUE(f.quality() > 0.9f);
  TEST_ASSERT_TRUE(f.quality() <= 1.0f);
  // Huge alternating noise (gate disabled so it reaches the estimator):
  // sd ~ 50 uT -> q ~ 0.01.
  for (int i = 0; i < 200; ++i) f.update(i % 2 ? 60.0f : -40.0f, o);
  const float q = f.quality();
  TEST_ASSERT_TRUE(q > 0.0f && q <= 1.0f);
  TEST_ASSERT_TRUE(q < 0.2f);
}

static void test_axis_reset_clears_state(void) {
  AxisFilter f;
  f.configure(false, false, false, 1.0f, 4.0f);
  bool o = false;
  for (int i = 0; i < 20; ++i) f.update(10.0f, o);
  f.setBaseline(4.0f);
  f.reset();
  TEST_ASSERT_FALSE(f.hasBaseline());
  // After reset the chain reprimes on the next sample: output equals input.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.5f, f.update(7.5f, o));
}

// -------------------------------- Vec3Filter -----------------------------

static void test_vec3filter_pipeline(void) {
  Vec3Filter vf;
  vf.configure(false, false, false, 1.0f, 0.0f);  // pass-through
  bool o = false;
  vf.x().update(3.0f, o);
  vf.y().update(4.0f, o);
  vf.z().update(0.0f, o);
  vf.setBaseline(1.0f, 1.0f, 0.0f);
  // Baseline applies per axis: x -> 2, y -> 3.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, vf.x().update(3.0f, o));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, vf.y().update(4.0f, o));
}

// --------------------------------- main ----------------------------------

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_lowpass_converges_to_step);
  RUN_TEST(test_lowpass_first_sample_primes);
  RUN_TEST(test_moving_average_window_accuracy);
  RUN_TEST(test_median_rejects_single_spike);
  RUN_TEST(test_noise_estimator_mean_stddev);
  RUN_TEST(test_axis_outlier_gate_holds_last);
  RUN_TEST(test_axis_baseline_subtraction);
  RUN_TEST(test_axis_tracks_sustained_field_change_after_baseline);
  RUN_TEST(test_axis_median_blocks_spike_in_chain);
  RUN_TEST(test_axis_moving_average_in_chain);
  RUN_TEST(test_axis_quality_bounds);
  RUN_TEST(test_axis_reset_clears_state);
  RUN_TEST(test_vec3filter_pipeline);
  return UNITY_END();
}
