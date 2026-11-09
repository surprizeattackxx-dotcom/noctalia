#include "notification/quiet_hours.h"
#include "tests/test_check.h"

int main() {
  using quiet_hours::contains;
  using quiet_hours::parseClockMinutes;

  TEST_CHECK(parseClockMinutes("22:00") == 22 * 60);
  TEST_CHECK(parseClockMinutes("7:05") == 7 * 60 + 5);
  TEST_CHECK(parseClockMinutes("00:00") == 0);
  TEST_CHECK(parseClockMinutes("23:59") == 23 * 60 + 59);
  TEST_CHECK(!parseClockMinutes("24:00").has_value());
  TEST_CHECK(!parseClockMinutes("12:60").has_value());
  TEST_CHECK(!parseClockMinutes("12:5").has_value());
  TEST_CHECK(!parseClockMinutes("1200").has_value());
  TEST_CHECK(!parseClockMinutes("").has_value());
  TEST_CHECK(!parseClockMinutes("ab:cd").has_value());
  TEST_CHECK(!parseClockMinutes("10:00 PM").has_value());

  // Overnight window 22:00 to 07:00.
  const int start = 22 * 60;
  const int end = 7 * 60;
  TEST_CHECK(contains(start, end, 22 * 60));
  TEST_CHECK(contains(start, end, 23 * 60 + 59));
  TEST_CHECK(contains(start, end, 0));
  TEST_CHECK(contains(start, end, 6 * 60 + 59));
  TEST_CHECK(!contains(start, end, 7 * 60));
  TEST_CHECK(!contains(start, end, 12 * 60));
  TEST_CHECK(!contains(start, end, 21 * 60 + 59));

  // Same-day window 13:00 to 14:30.
  TEST_CHECK(contains(13 * 60, 14 * 60 + 30, 13 * 60));
  TEST_CHECK(contains(13 * 60, 14 * 60 + 30, 14 * 60 + 29));
  TEST_CHECK(!contains(13 * 60, 14 * 60 + 30, 14 * 60 + 30));
  TEST_CHECK(!contains(13 * 60, 14 * 60 + 30, 12 * 60 + 59));

  // Equal start and end is empty, not a full day.
  TEST_CHECK(!contains(8 * 60, 8 * 60, 8 * 60));
  return 0;
}
