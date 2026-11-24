// totals: the app's uptime and idle time, counted on between its updates.
//   pio test -d firmware -e native -f test_totals
#include <unity.h>
#include <string.h>
#include "totals.h"

void setUp(void) { totals_reset(); }
void tearDown(void) {}

static void test_unknown_until_set(void) {
    char buf[48];
    totals_line(true, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
    totals_set("junk", 0);
    totals_set("100,200,i", 0);     // idle beyond uptime
    totals_set("100,50,x", 0);
    TEST_ASSERT_FALSE(totals_known());
}

static void test_formats(void) {
    char buf[48];
    totals_fmt(725, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("12h05m", buf);
    totals_fmt(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0h00m", buf);
    totals_fmt(60 * 1234 + 59, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1234h59m", buf);
    totals_link(true, 0);
    totals_set("43500,12000,w", 0);          // 12h05m up, 3h20m idle
    totals_line(true, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("12h05m/3h20m", buf);
    totals_line(false, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Uptime: 12h05m / Idle: 3h20m", buf);
}

static void test_counts_on_while_linked(void) {
    totals_link(true, 1000);
    totals_set("600,120,w", 1000);
    // Working: only the uptime moves.
    TEST_ASSERT_EQUAL_UINT32(15, totals_uptime_min(1000 + 5 * 60000));
    TEST_ASSERT_EQUAL_UINT32(2, totals_idle_min(1000 + 5 * 60000));
    // Idle: both.
    totals_set("900,120,i", 301000);
    TEST_ASSERT_EQUAL_UINT32(20, totals_uptime_min(301000 + 5 * 60000));
    TEST_ASSERT_EQUAL_UINT32(7, totals_idle_min(301000 + 5 * 60000));
    // The link goes: they stand still, and go on when it is back.
    totals_link(false, 601000);
    TEST_ASSERT_EQUAL_UINT32(20, totals_uptime_min(601000 + 60 * 60000));
    totals_link(true, 4201000);
    TEST_ASSERT_EQUAL_UINT32(21, totals_uptime_min(4201000 + 60000));
    TEST_ASSERT_EQUAL_UINT32(8, totals_idle_min(4201000 + 60000));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_unknown_until_set);
    RUN_TEST(test_formats);
    RUN_TEST(test_counts_on_while_linked);
    return UNITY_END();
}
