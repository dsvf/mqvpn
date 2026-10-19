// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * mqvpn_budget.h — pure metered-path budget accounting (xquic-free).
 *
 * Phase 1 of the traffic-shaping policy. Deliberately I/O-free and
 * xquic-free (mirrors flow_sched.h / mqvpn_path_label.h) so the accounting
 * math and its persisted-state wire format are unit-testable without linking
 * xquic, and so the library core never touches a file or the wall clock.
 *
 * The model:
 *   - A metered path has a monthly allowance (bytes) and a ResetDay
 *     (calendar day, 1..31) on which the allowance resets.
 *   - The billing period is the window [ResetDay of month M, ResetDay of
 *     month M+1); ResetDay is clamped to the month length (a 31 in a 30-day
 *     month resets on the 30th).
 *   - The path is *exhausted* once used >= allowance in the current period
 *     (hard stop: never spend more than the allowance).
 *   - Separately, the path is eligible for *speed-boost* (use-it-or-lose-it
 *     data) when it is behind schedule: used < RAMP_FRACTION_PCT% of the
 *     linearly prorated target (day_of_period / period_days x allowance).
 *     This is the user's rule: "if used on day x is less than 70% of
 *     (x/period) x allowance, enable the link for general speed".
 *   - The congestion/failover trigger (Phase 2, the scheduler) overrides the
 *     ramp: a genuinely slow primary still spends the metered path until it
 *     is *exhausted*, regardless of speed_boost_allowed.
 *
 * The caller (CLI) owns time and persistence: it passes the civil "now" and
 * the persisted state in/out. The two gates (exhausted, speed_boost_allowed)
 * are the only decisions the scheduler layer later consumes.
 */
#ifndef MQVPN_BUDGET_H
#define MQVPN_BUDGET_H

#include <stddef.h>
#include <stdint.h>

/* Percent of the prorated target below which the metered path may be engaged
 * for general speed improvement (use-it-or-lose-it). Matches the user's
 * "70% of linear usage" rule. */
#define MQVPN_BUDGET_RAMP_FRACTION_PCT 70

/* Persisted state for one metered path. `period` is an opaque equality key
 * (see mqvpn_budget_period_key); 0 = never billed. `used_bytes` is the
 * metered path's sent bytes accumulated so far this period. */
typedef struct {
    uint64_t period;   /* opaque period key; 0 = unset */
    uint64_t used_bytes;
} mqvpn_budget_state_t;

/* Civil date, caller-supplied (so this module stays I/O- and timezone-free). */
typedef struct {
    int year;  /* e.g. 2026 */
    int month; /* 1..12 */
    int day;   /* 1..31 */
} mqvpn_budget_date_t;

/* Result of assessing a metered path's budget. */
typedef struct {
    uint64_t period_key;           /* current billing period key */
    uint64_t used_bytes;           /* used within the current period (0 on a fresh period) */
    uint64_t allowance_bytes;
    uint64_t remaining_bytes;      /* allowance - used, floor 0 */
    uint64_t prorated_target_bytes; /* day_of_period/period_days x allowance */
    int      day_of_period;        /* 1-based day into the period */
    int      period_days;          /* length of the period in days */
    int      exhausted;            /* remaining == 0 (hard stop) */
    int      speed_boost_allowed;  /* behind schedule: used < ramp fraction of prorated target */
} mqvpn_budget_assessment_t;

/* MB -> bytes. MonthlyAllowanceMB is stored in the config; sent bytes are
 * counted in bytes, so convert once at the assess() call site. */
static inline uint64_t
mqvpn_budget_mb_to_bytes(uint64_t mb)
{
    return mb * 1024ull * 1024ull;
}

/* Days in month (leap-aware). Returns 0 for an out-of-range month. */
int mqvpn_budget_days_in_month(int year, int month);

/* Opaque key identifying the billing period that contains `now` for a path
 * whose allowance resets on `reset_day`. Stable within a period and strictly
 * changing across period boundaries; do not decode it, only compare it. */
uint64_t mqvpn_budget_period_key(int reset_day, const mqvpn_budget_date_t *now);

/* Assess a metered path's budget. `allowance_bytes == 0` reports exhausted
 * (callers should treat unlimited paths by never calling this). A persisted
 * `state` whose `period` does not match the current period starts fresh
 * (used_bytes = 0). */
void mqvpn_budget_assess(uint64_t allowance_bytes, int reset_day,
                         const mqvpn_budget_state_t *state,
                         const mqvpn_budget_date_t *now,
                         mqvpn_budget_assessment_t *out);

/* Encode state into a single line ("<period> <used_bytes>"), NUL-terminated.
 * Returns bytes written excluding the NUL, or -1 if buf is too small. */
int mqvpn_budget_state_encode(char *buf, size_t cap, const mqvpn_budget_state_t *st);

/* Decode a line produced by mqvpn_budget_state_encode(). Returns 0 on
 * success, -1 on malformed input. */
int mqvpn_budget_state_decode(const char *s, mqvpn_budget_state_t *st);

#endif /* MQVPN_BUDGET_H */
