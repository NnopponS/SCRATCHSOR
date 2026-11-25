#include <unity.h>
#include <cstring>
#include "services/gap_policy.h"
using namespace scratchsor;
void setUp() {}
void tearDown() {}

void test_one_sensor_requires_confirmed_fixture() {
  GapConfig g;
  TEST_ASSERT_FALSE(gapModelUsable(g, gapSignature(g), 1, 1));
  g.fixedGeometry = true;
  TEST_ASSERT_TRUE(gapModelUsable(g, gapSignature(g), 1, 1));
}

void test_geometry_changes_reject_previous_model() {
  GapConfig g;
  g.fixedGeometry = true;
  strcpy(g.setup, "opposite poles on fixed rail");
  const auto signature = gapSignature(g);
  g.reference = 1;
  TEST_ASSERT_FALSE(gapModelUsable(g, signature, 1, 1));
  g.reference = 0;
  g.sensorId = 1;
  TEST_ASSERT_FALSE(gapModelUsable(g, signature, 2, 2));
  g.sensorId = 0;
  strcpy(g.setup, "different magnets");
  TEST_ASSERT_FALSE(gapModelUsable(g, signature, 1, 1));
}

void test_sensor_dropout_cannot_reuse_model() {
  GapConfig g;
  g.fixedGeometry = true;
  TEST_ASSERT_FALSE(gapModelUsable(g, gapSignature(g), 1, 0));
  TEST_ASSERT_FALSE(gapModelUsable(g, gapSignature(g), 3, 1));
  TEST_ASSERT_FALSE(gapModelUsable(g, gapSignature(g), 0, 0));
}

void test_reference_rejects_unusable_measurements() {
  TEST_ASSERT_FALSE(gapReferenceAllowed(NAN));
  TEST_ASSERT_FALSE(gapReferenceAllowed(INFINITY));
  TEST_ASSERT_FALSE(gapReferenceAllowed(0));
  TEST_ASSERT_FALSE(gapReferenceAllowed(-1));
  TEST_ASSERT_FALSE(gapReferenceAllowed(501));
  TEST_ASSERT_TRUE(gapReferenceAllowed(10));
  TEST_ASSERT_TRUE(gapReferenceAllowed(500));
}

void test_validation_cannot_pass_without_independent_points() {
  TEST_ASSERT_FALSE(gapValidationPass(0, 0, 0));
  TEST_ASSERT_FALSE(gapValidationPass(1, 0, 0));
  TEST_ASSERT_FALSE(gapValidationPass(2, NAN, 0));
  TEST_ASSERT_FALSE(gapValidationPass(2, 5.1f, 6));
  TEST_ASSERT_FALSE(gapValidationPass(2, 4, 16));
  TEST_ASSERT_TRUE(gapValidationPass(2, 4, 6));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_one_sensor_requires_confirmed_fixture);
  RUN_TEST(test_geometry_changes_reject_previous_model);
  RUN_TEST(test_sensor_dropout_cannot_reuse_model);
  RUN_TEST(test_reference_rejects_unusable_measurements);
  RUN_TEST(test_validation_cannot_pass_without_independent_points);
  return UNITY_END();
}
