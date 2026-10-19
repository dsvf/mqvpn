// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * test_budget.c — unit tests for the metered-path budget module
 * (src/mqvpn_budget.h/.c). xquic-free; see CMakeLists.txt test_budget.
 *
 * Build: cc -o tests/test_budget tests/test_budget.c src/mqvpn_budget.c
 */

#include "mqvpn_budget.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define ASSERT_EQ_INT(a, b, msg)                                               \
    do {                                                                       \
        if ((a) == (b)) {                                                      \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            fprintf(stderr, "FAIL [%s]: %d != %d\n", msg, (int)(a), (int)(b)); \
        }                                                                      \
    } while (0)

#define ASSERT_EQ_STR(a, b, msg)                                         \
    do {                                                                 \
        if (strcmp((a), (b)) == 0) {                                     \
            g_pass++;                                                    \
        } else {                                                         \
            g_fail++;                                                    \
            fprintf(stderr, "FAIL [%s]: '%s' != '%s'\n", msg, (a), (b)); \
        }                                                                \
    } while (0)

#define ASSERT_EQ_ULL(a, b, msg)                                                       \
    do {                                                                               \
        if ((a) == (b)) {                                                              \
            g_pass++;                                                                  \
        } else {                                                                       \
            g_fail++;                                                                  \
            fprintf(stderr, "FAIL [%s]: %llu != %llu\n", msg, (unsigned long long)(a), \
                    (unsigned long long)(b));                                          \
        }                                                                              \
    } while (0)

static void
test_days_in_month(void)
{
    ASSERT_EQ_INT(mqvpn_budget_days_in_month(2026, 1), 31, "jan");
    ASSERT_EQ_INT(mqvpn_budget_days_in_month(2026, 4), 30, "apr");
    ASSERT_EQ_INT(mqvpn_budget_days_in_month(2023, 2), 28, "feb non-leap");
    ASSERT_EQ_INT(mqvpn_budget_days_in_month(2024, 2), 29, "feb leap");
    ASSERT_EQ_INT(mqvpn_budget_days_in_month(2000, 2), 29, "feb 400-leap");
    ASSERT_EQ_INT(mqvpn_budget_days_in_month(1900, 2), 28, "feb 100-non-leap");
    ASSERT_EQ_INT(mqvpn_budget_days_in_month(2026, 13), 0, "month 13 invalid");
}

static void
test_period_key(void)
{
    mqvpn_budget_date_t now = {2026, 3, 20};
    /* reset day 15: March 20 is >= 15, so the period started March 2026. */
    ASSERT_EQ_ULL(mqvpn_budget_period_key(15, &now), (uint64_t)2026 * 100 + 3,
                  "reset15 mar20 -> mar");

    now.day = 10; /* before the 15th -> previous month */
    ASSERT_EQ_ULL(mqvpn_budget_period_key(15, &now), (uint64_t)2026 * 100 + 2,
                  "reset15 mar10 -> feb");

    now.year = 2026; now.month = 1; now.day = 10; /* before Jan 15 -> Dec 2025 */
    ASSERT_EQ_ULL(mqvpn_budget_period_key(15, &now), (uint64_t)2025 * 100 + 12,
                  "reset15 jan10 -> dec");

    /* reset day 31 clamped in a 30-day month */
    now.year = 2026; now.month = 4; now.day = 30; /* April 30 == effective reset */
    ASSERT_EQ_ULL(mqvpn_budget_period_key(31, &now), (uint64_t)2026 * 100 + 4,
                  "reset31 apr30 -> apr (clamped)");
}

static void
test_assess_fresh_period(void)
{
    mqvpn_budget_state_t st = {0, 0};
    mqvpn_budget_date_t now = {2026, 4, 1}; /* reset day 1, April (30 days) */
    mqvpn_budget_assessment_t a;
    mqvpn_budget_assess(1000, 1, &st, &now, &a);

    ASSERT_EQ_ULL(a.period_key, (uint64_t)2026 * 100 + 4, "fresh period key");
    ASSERT_EQ_ULL(a.used_bytes, 0, "fresh period used 0");
    ASSERT_EQ_ULL(a.allowance_bytes, 1000, "allowance");
    ASSERT_EQ_ULL(a.remaining_bytes, 1000, "remaining");
    ASSERT_EQ_INT(a.day_of_period, 1, "day of period 1");
    ASSERT_EQ_INT(a.period_days, 30, "period days 30");
    ASSERT_EQ_INT(a.exhausted, 0, "not exhausted");
    ASSERT_EQ_INT(a.speed_boost_allowed, 1, "behind schedule on day 1");
}

static void
test_assess_ramp_boundary(void)
{
    /* April 2026, reset day 1. day 15 of 30: prorated = 1000*15/30 = 500,
     * ramp threshold = 350. */
    mqvpn_budget_date_t now = {2026, 4, 15};

    mqvpn_budget_state_t below = {(uint64_t)2026 * 100 + 4, 349};
    mqvpn_budget_assessment_t a;
    mqvpn_budget_assess(1000, 1, &below, &now, &a);
    ASSERT_EQ_INT(a.day_of_period, 15, "day 15");
    ASSERT_EQ_ULL(a.prorated_target_bytes, 500, "prorated 500");
    ASSERT_EQ_INT(a.speed_boost_allowed, 1, "349 < 350 allowed");

    mqvpn_budget_state_t at = {(uint64_t)2026 * 100 + 4, 350};
    mqvpn_budget_assess(1000, 1, &at, &now, &a);
    ASSERT_EQ_INT(a.speed_boost_allowed, 0, "350 >= 350 not allowed");
    ASSERT_EQ_INT(a.exhausted, 0, "350 < 1000 not exhausted");
}

static void
test_assess_exhausted(void)
{
    mqvpn_budget_date_t now = {2026, 4, 20};
    mqvpn_budget_state_t st = {(uint64_t)2026 * 100 + 4, 1000};
    mqvpn_budget_assessment_t a;
    mqvpn_budget_assess(1000, 1, &st, &now, &a);
    ASSERT_EQ_INT(a.exhausted, 1, "exhausted");
    ASSERT_EQ_ULL(a.remaining_bytes, 0, "remaining floor 0");
    ASSERT_EQ_INT(a.speed_boost_allowed, 0, "exhausted never speed-boosts");
}

static void
test_assess_period_rollover(void)
{
    /* Persisted state from a previous period must reset used to 0. */
    mqvpn_budget_date_t now = {2026, 5, 5}; /* new period (May) */
    mqvpn_budget_state_t stale = {(uint64_t)2026 * 100 + 4, 999};
    mqvpn_budget_assessment_t a;
    mqvpn_budget_assess(1000, 1, &stale, &now, &a);
    ASSERT_EQ_ULL(a.used_bytes, 0, "rollover resets used");
    ASSERT_EQ_ULL(a.remaining_bytes, 1000, "rollover full remaining");
    ASSERT_EQ_ULL(a.period_key, (uint64_t)2026 * 100 + 5, "rollover new key");
}

static void
test_assess_zero_allowance(void)
{
    mqvpn_budget_date_t now = {2026, 4, 1};
    mqvpn_budget_state_t st = {0, 0};
    mqvpn_budget_assessment_t a;
    mqvpn_budget_assess(0, 1, &st, &now, &a);
    ASSERT_EQ_INT(a.exhausted, 1, "zero allowance exhausted");
}

static void
test_assess_leap_feb(void)
{
    mqvpn_budget_date_t now = {2024, 2, 10};
    mqvpn_budget_state_t st = {0, 0};
    mqvpn_budget_assessment_t a;
    mqvpn_budget_assess(2900, 1, &st, &now, &a);
    ASSERT_EQ_INT(a.period_days, 29, "leap feb period 29");
    ASSERT_EQ_INT(a.day_of_period, 10, "leap feb day 10");
}

static void
test_state_roundtrip(void)
{
    mqvpn_budget_state_t st = {20260004, 123456789012ull};
    char buf[64];
    int n = mqvpn_budget_state_encode(buf, sizeof(buf), &st);
    ASSERT_EQ_INT(n > 0, 1, "encode length");
    ASSERT_EQ_STR(buf, "20260004 123456789012", "encode text");

    mqvpn_budget_state_t out = {0, 0};
    ASSERT_EQ_INT(mqvpn_budget_state_decode(buf, &out), 0, "decode ok");
    ASSERT_EQ_ULL(out.period, st.period, "decode period");
    ASSERT_EQ_ULL(out.used_bytes, st.used_bytes, "decode used");
}

static void
test_state_decode_malformed(void)
{
    mqvpn_budget_state_t out;
    ASSERT_EQ_INT(mqvpn_budget_state_decode("", &out), -1, "empty fails");
    ASSERT_EQ_INT(mqvpn_budget_state_decode("garbage", &out), -1, "non-numeric fails");
    ASSERT_EQ_INT(mqvpn_budget_state_decode("123", &out), -1, "single field fails");
    ASSERT_EQ_INT(mqvpn_budget_state_decode("123 456 junk", &out), -1, "trailing junk fails");
}

int
main(void)
{
    test_days_in_month();
    test_period_key();
    test_assess_fresh_period();
    test_assess_ramp_boundary();
    test_assess_exhausted();
    test_assess_period_rollover();
    test_assess_zero_allowance();
    test_assess_leap_feb();
    test_state_roundtrip();
    test_state_decode_malformed();

    printf("\n=== test_budget: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
