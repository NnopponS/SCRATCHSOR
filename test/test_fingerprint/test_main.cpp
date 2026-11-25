#include <unity.h>
#include <cmath>
#include "services/fingerprint.h"
#include "../../src/services/fingerprint.cpp"
using namespace scratchsor;
void setUp() {}
void tearDown() {}
void test_thresholds() {
  const float lo[] = {40, 0, 50, 0}, hi[] = {60, 30, 70, 60};
  for (unsigned p=0; p<4; ++p) {
    TEST_ASSERT_EQUAL(0, reachCategory(p, lo[p] - .01f));
    TEST_ASSERT_EQUAL(1, reachCategory(p, lo[p]));
    TEST_ASSERT_EQUAL(1, reachCategory(p, hi[p] - .01f));
    TEST_ASSERT_EQUAL(2, reachCategory(p, hi[p]));
    TEST_ASSERT_EQUAL(0, reachCategory(p, -100));
  }
  TEST_ASSERT_TRUE(referenceAllowed(-500)); TEST_ASSERT_TRUE(referenceAllowed(0));
  TEST_ASSERT_TRUE(referenceAllowed(500)); TEST_ASSERT_FALSE(referenceAllowed(501));
  TEST_ASSERT_FALSE(referenceAllowed(NAN)); TEST_ASSERT_FALSE(referenceAllowed(INFINITY));
}
void populate(FingerprintData& d) {
  for (unsigned c=0; c<3; ++c) for (unsigned j=0; j<10; ++j) {
    FingerprintPoint p{}; p.profile=0; p.id=d.nextId++; p.test=j>=8;
    p.referenceMm=c==0?0:c==1?50:80;
    for (auto& x:p.features) x=float(c*100+j);
    TEST_ASSERT_TRUE(appendFingerprint(d,p));
  }
}
void test_fit_and_separate_validation() {
  FingerprintData d{}; populate(d);
  for (unsigned i=0;i<d.count;++i) if(d.points[i].test)
    for(auto& x:d.points[i].features) x=100000;
  TEST_ASSERT_TRUE(fitFingerprint(d,0));
  TEST_ASSERT_FLOAT_WITHIN(.001f,103.5f,d.models[0].mean[0]);
  TEST_ASSERT_TRUE(d.models[0].ready); // low accuracy remains advisory
  TEST_ASSERT_FLOAT_WITHIN(.001f,1.f/3,d.models[0].accuracy);
  TEST_ASSERT_FALSE(d.models[1].ready);
  float x[12]{}; float vote=0;
  TEST_ASSERT_EQUAL(0,predictFingerprint(d,0,x,vote));
  x[0]=NAN; TEST_ASSERT_EQUAL(-1,predictFingerprint(d,0,x,vote));
  TEST_ASSERT_TRUE(deleteFingerprint(d,1)); TEST_ASSERT_FALSE(d.models[0].ready);
  TEST_ASSERT_FALSE(fitFingerprint(d,0)); // now fewer than 8 train for class 0
}
void test_window_and_capacity() {
  FeatureWindow w; float x[12]{}, out[12];
  for(unsigned i=0;i<20;++i) {for(auto& v:x)v=float(i); TEST_ASSERT_EQUAL(i==19,w.push(x,out));}
  TEST_ASSERT_FLOAT_WITHIN(.001f,9.5f,out[0]);
  for(auto& v:x)v=20; TEST_ASSERT_TRUE(w.push(x,out));
  TEST_ASSERT_FLOAT_WITHIN(.001f,10.5f,out[0]);
  w.clear(); TEST_ASSERT_FALSE(w.push(x,out));
  FingerprintData d{}; FingerprintPoint p{};
  for(unsigned i=0;i<128;++i) {p.id=i+1; TEST_ASSERT_TRUE(appendFingerprint(d,p));}
  p.id=129; TEST_ASSERT_FALSE(appendFingerprint(d,p)); TEST_ASSERT_EQUAL(128,d.count);
  TEST_ASSERT_TRUE(deleteFingerprint(d,64)); TEST_ASSERT_EQUAL(127,d.count);
  TEST_ASSERT_TRUE(appendFingerprint(d,p));
}
void test_three_way_tie_and_no_rejection() {
  FingerprintData d{};
  for(unsigned c=0;c<3;++c) {FingerprintPoint p{};p.id=c+1;
    p.referenceMm=c==0?0:c==1?50:80;p.features[0]=float(c+1);
    TEST_ASSERT_TRUE(appendFingerprint(d,p));}
  auto& m=d.models[0]; m.ready=true; for(auto& s:m.scale)s=1;
  float x[12]{},conf;
  TEST_ASSERT_EQUAL(0,predictFingerprint(d,0,x,conf));
  x[0]=100000; TEST_ASSERT_EQUAL(2,predictFingerprint(d,0,x,conf));
}
int main() {UNITY_BEGIN(); RUN_TEST(test_thresholds); RUN_TEST(test_fit_and_separate_validation);
  RUN_TEST(test_window_and_capacity); RUN_TEST(test_three_way_tie_and_no_rejection); return UNITY_END();}
