#include <unity.h>
#include <cstring>
#include <initializer_list>
#include "networking/usb_protocol.h"

using scratchsor::UsbLineBuffer;
void setUp() {}
void tearDown() {}

void test_partial_lines_and_crlf() {
  UsbLineBuffer<12> b;
  for (char c : {'@', 'U', 'S', 'B', ' ', '{', '}', '\r'})
    TEST_ASSERT_FALSE(b.push(c));
  TEST_ASSERT_TRUE(b.push('\n'));
  TEST_ASSERT_EQUAL_STRING("@USB {}\r", b.line());
  TEST_ASSERT_FALSE(b.push('x'));
  TEST_ASSERT_TRUE(b.push('\n'));
  TEST_ASSERT_EQUAL_STRING("x", b.line());
}

void test_overflow_discards_entire_line_and_recovers() {
  UsbLineBuffer<4> b;
  for (char c : {'a', 'b', 'c', 'd', 'e', '{', '}'})
    TEST_ASSERT_FALSE(b.push(c));
  TEST_ASSERT_FALSE(b.push('\n'));
  for (char c : {'o', 'k'}) TEST_ASSERT_FALSE(b.push(c));
  TEST_ASSERT_TRUE(b.push('\n'));
  TEST_ASSERT_EQUAL_STRING("ok", b.line());
}

void test_abandoned_partial_request_cannot_execute_its_tail() {
  UsbLineBuffer<16> b;
  b.push('{');
  b.discard();
  b.push('}');
  TEST_ASSERT_FALSE(b.push('\n'));
  b.push('x');
  TEST_ASSERT_TRUE(b.push('\n'));
  TEST_ASSERT_EQUAL_STRING("x", b.line());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_partial_lines_and_crlf);
  RUN_TEST(test_overflow_discards_entire_line_and_recovers);
  RUN_TEST(test_abandoned_partial_request_cannot_execute_its_tail);
  return UNITY_END();
}
