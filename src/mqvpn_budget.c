// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

#include "mqvpn_budget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Days since 0000-03-01 (Howard Hinnant's civil<->days algorithm). Only
 * differences are ever used, so the epoch offset is irrelevant. */
static int64_t
days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int
is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int
mqvpn_budget_days_in_month(int year, int month)
{
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap(year)) return 29;
    return dim[month - 1];
}

static void
prev_month(int *y, int *m)
{
    if (*m == 1) {
        *m = 12;
        (*y)--;
    } else {
        (*m)--;
    }
}

static void
next_month(int *y, int *m)
{
    if (*m == 12) {
        *m = 1;
        (*y)++;
    } else {
        (*m)++;
    }
}

/* ResetDay clamped to the month length (a 31 in a 30-day month → 30). */
static int
effective_reset_day(int year, int month, int reset_day)
{
    int dim = mqvpn_budget_days_in_month(year, month);
    return reset_day < dim ? reset_day : dim;
}

uint64_t
mqvpn_budget_period_key(int reset_day, const mqvpn_budget_date_t *now)
{
    if (!now) return 0;
    int y = now->year, m = now->month;
    int rd = effective_reset_day(y, m, reset_day);
    int64_t nowd = days_from_civil(now->year, now->month, now->day);
    if (nowd < days_from_civil(y, m, rd))
        prev_month(&y, &m);
    return (uint64_t)y * 100 + (uint64_t)m;
}

void
mqvpn_budget_assess(uint64_t allowance_bytes, int reset_day,
                    const mqvpn_budget_state_t *state, const mqvpn_budget_date_t *now,
                    mqvpn_budget_assessment_t *out)
{
    memset(out, 0, sizeof(*out));
    out->allowance_bytes = allowance_bytes;
    if (!now || allowance_bytes == 0 || reset_day < 1 || reset_day > 31) {
        out->exhausted = 1;
        return;
    }

    /* Find the billing period that contains `now`. */
    int y = now->year, m = now->month;
    int rd = effective_reset_day(y, m, reset_day);
    int64_t nowd = days_from_civil(now->year, now->month, now->day);
    int64_t startd = days_from_civil(y, m, rd);
    if (nowd < startd) {
        prev_month(&y, &m);
        rd = effective_reset_day(y, m, reset_day);
        startd = days_from_civil(y, m, rd);
    }

    int ny = y, nm = m;
    next_month(&ny, &nm);
    int nrd = effective_reset_day(ny, nm, reset_day);
    int64_t nextd = days_from_civil(ny, nm, nrd);

    int period_days = (int)(nextd - startd);
    int day_of_period = (int)(nowd - startd) + 1; /* 1-based */
    uint64_t period_key = (uint64_t)y * 100 + (uint64_t)m;

    uint64_t used = (state && state->period == period_key) ? state->used_bytes : 0;
    uint64_t remaining = allowance_bytes > used ? allowance_bytes - used : 0;
    uint64_t prorated = (allowance_bytes * (uint64_t)day_of_period) /
                        (uint64_t)(period_days > 0 ? period_days : 1);
    uint64_t ramp_threshold = prorated * MQVPN_BUDGET_RAMP_FRACTION_PCT / 100;

    out->period_key = period_key;
    out->used_bytes = used;
    out->remaining_bytes = remaining;
    out->prorated_target_bytes = prorated;
    out->day_of_period = day_of_period;
    out->period_days = period_days;
    out->exhausted = (remaining == 0);
    out->speed_boost_allowed = !out->exhausted && used < ramp_threshold;
}

int
mqvpn_budget_state_encode(char *buf, size_t cap, const mqvpn_budget_state_t *st)
{
    if (!buf || !st || cap == 0) return -1;
    int n = snprintf(buf, cap, "%llu %llu", (unsigned long long)st->period,
                     (unsigned long long)st->used_bytes);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

int
mqvpn_budget_state_decode(const char *s, mqvpn_budget_state_t *st)
{
    if (!s || !st) return -1;
    char *end = NULL;
    unsigned long long p = strtoull(s, &end, 10);
    if (end == s) return -1;
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end == '\0') return -1; /* need a second field */
    char *end2 = NULL;
    unsigned long long u = strtoull(end, &end2, 10);
    if (end2 == end) return -1;
    while (*end2 == ' ' || *end2 == '\t' || *end2 == '\r' || *end2 == '\n')
        end2++;
    if (*end2 != '\0') return -1; /* trailing junk */

    st->period = (uint64_t)p;
    st->used_bytes = (uint64_t)u;
    return 0;
}
