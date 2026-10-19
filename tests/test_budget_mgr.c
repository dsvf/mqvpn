// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * test_budget_mgr.c — unit tests for the CLI-side budget manager
 * (src/budget_mgr.h/.c). xquic-free; see CMakeLists.txt test_budget_mgr.
 */

#include "budget_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define ASSERT_EQ_STR(a, b, msg)                                         \
    do {                                                                 \
        if (strcmp((a), (b)) == 0) {                                     \
            g_pass++;                                                    \
        } else {                                                         \
            g_fail++;                                                    \
            fprintf(stderr, "FAIL [%s]: '%s' != '%s'\n", msg, (a), (b)); \
        }                                                                \
    } while (0)

static const mqvpn_budget_date_t g_now_apr = {2026, 4, 15};

static void
test_init_and_find(void)
{
    mqvpn_path_role_t roles[2];
    memset(roles, 0, sizeof(roles));
    snprintf(roles[0].iface, sizeof(roles[0].iface), "wwan0");
    roles[0].metered = 1;
    roles[0].monthly_allowance_mb = 50000;
    roles[0].reset_day = 1;
    snprintf(roles[1].iface, sizeof(roles[1].iface), "eth0");

    mqvpn_budget_mgr_t mgr;
    mqvpn_budget_mgr_init(&mgr, roles, 2);
    ASSERT_EQ_INT(mgr.n_roles, 2, "init copies 2 roles");
    ASSERT_EQ_STR(mgr.roles[0].iface, "wwan0", "init role[0] iface");
    ASSERT_EQ_INT(mgr.roles[0].metered, 1, "init role[0] metered");
    ASSERT_EQ_INT(mqvpn_budget_mgr_find(&mgr, "wwan0"), 0, "find wwan0");
    ASSERT_EQ_INT(mqvpn_budget_mgr_find(&mgr, "eth0"), 1, "find eth0");
    ASSERT_EQ_INT(mqvpn_budget_mgr_find(&mgr, "nope"), -1, "find unknown");
}

static void
test_apply_accumulates(void)
{
    mqvpn_path_role_t roles[1];
    memset(roles, 0, sizeof(roles));
    snprintf(roles[0].iface, sizeof(roles[0].iface), "wwan0");
    roles[0].metered = 1;
    roles[0].monthly_allowance_mb = 100;
    roles[0].reset_day = 1;

    mqvpn_budget_mgr_t mgr;
    mqvpn_budget_mgr_init(&mgr, roles, 1);
    mqvpn_budget_assessment_t a;

    mqvpn_budget_mgr_apply(&mgr, "wwan0", 0, &g_now_apr, &a);
    ASSERT_EQ_ULL(a.used_bytes, 0, "first apply used 0");

    mqvpn_budget_mgr_apply(&mgr, "wwan0", 1000, &g_now_apr, &a);
    ASSERT_EQ_ULL(a.used_bytes, 1000, "used 1000 after delta");

    mqvpn_budget_mgr_apply(&mgr, "wwan0", 2500, &g_now_apr, &a);
    ASSERT_EQ_ULL(a.used_bytes, 2500, "used 2500 after second delta");
}

static void
test_apply_period_rollover(void)
{
    mqvpn_path_role_t roles[1];
    memset(roles, 0, sizeof(roles));
    snprintf(roles[0].iface, sizeof(roles[0].iface), "wwan0");
    roles[0].metered = 1;
    roles[0].monthly_allowance_mb = 100;
    roles[0].reset_day = 1;

    mqvpn_budget_mgr_t mgr;
    mqvpn_budget_mgr_init(&mgr, roles, 1);
    mqvpn_budget_assessment_t a;

    mqvpn_budget_mgr_apply(&mgr, "wwan0", 2500, &g_now_apr, &a);
    ASSERT_EQ_ULL(a.used_bytes, 2500, "april used");

    mqvpn_budget_date_t may = {2026, 5, 5};
    mqvpn_budget_mgr_apply(&mgr, "wwan0", 2500, &may, &a);
    ASSERT_EQ_ULL(a.used_bytes, 0, "rollover resets used");
    ASSERT_EQ_ULL(a.period_key, (uint64_t)2026 * 100 + 5, "rollover to may");

    mqvpn_budget_mgr_apply(&mgr, "wwan0", 3000, &may, &a);
    ASSERT_EQ_ULL(a.used_bytes, 500, "post-rollover delta counted only");
}

static void
test_apply_counter_reset(void)
{
    mqvpn_path_role_t roles[1];
    memset(roles, 0, sizeof(roles));
    snprintf(roles[0].iface, sizeof(roles[0].iface), "wwan0");
    roles[0].metered = 1;
    roles[0].monthly_allowance_mb = 100;
    roles[0].reset_day = 1;

    mqvpn_budget_mgr_t mgr;
    mqvpn_budget_mgr_init(&mgr, roles, 1);
    mqvpn_budget_assessment_t a;

    mqvpn_budget_mgr_apply(&mgr, "wwan0", 5000, &g_now_apr, &a);
    ASSERT_EQ_ULL(a.used_bytes, 5000, "used 5000");

    /* Path slot re-created: session counter drops; do not bill backwards. */
    mqvpn_budget_mgr_apply(&mgr, "wwan0", 10, &g_now_apr, &a);
    ASSERT_EQ_ULL(a.used_bytes, 5000, "counter reset bills nothing");

    mqvpn_budget_mgr_apply(&mgr, "wwan0", 60, &g_now_apr, &a);
    ASSERT_EQ_ULL(a.used_bytes, 5050, "post-reset delta billed");
}

static void
test_non_metered_not_assessed(void)
{
    mqvpn_path_role_t roles[1];
    memset(roles, 0, sizeof(roles));
    snprintf(roles[0].iface, sizeof(roles[0].iface), "eth0"); /* metered = 0 */

    mqvpn_budget_mgr_t mgr;
    mqvpn_budget_mgr_init(&mgr, roles, 1);
    mqvpn_budget_assessment_t a;
    mqvpn_budget_mgr_apply(&mgr, "eth0", 999999, &g_now_apr, &a);
    ASSERT_EQ_ULL(a.used_bytes, 0, "unmetered leaves zeroed");
    ASSERT_EQ_INT(a.exhausted, 0, "unmetered not exhausted");
}

static void
test_save_load_roundtrip(void)
{
    mqvpn_path_role_t roles[2];
    memset(roles, 0, sizeof(roles));
    snprintf(roles[0].iface, sizeof(roles[0].iface), "wwan0");
    roles[0].metered = 1;
    roles[0].monthly_allowance_mb = 50000;
    roles[0].reset_day = 1;
    snprintf(roles[1].iface, sizeof(roles[1].iface), "eth0"); /* unmetered */

    mqvpn_budget_mgr_t mgr;
    mqvpn_budget_mgr_init(&mgr, roles, 2);
    mqvpn_budget_assessment_t a;
    mqvpn_budget_mgr_apply(&mgr, "wwan0", 123456, &g_now_apr, &a);

    char path[] = "/tmp/test_budget_mgr_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    unlink(path);

    ASSERT_EQ_INT(mqvpn_budget_mgr_save(&mgr, path), 0, "save ok");

    mqvpn_budget_mgr_t mgr2;
    mqvpn_budget_mgr_init(&mgr2, roles, 2);
    ASSERT_EQ_INT(mqvpn_budget_mgr_load(&mgr2, path), 0, "load ok");

    mqvpn_budget_assessment_t b;
    mqvpn_budget_mgr_assess(&mgr2, "wwan0", &g_now_apr, &b);
    ASSERT_EQ_ULL(b.used_bytes, 123456, "roundtrip used preserved");

    unlink(path);
}

static void
test_load_missing_file(void)
{
    mqvpn_path_role_t roles[1];
    memset(roles, 0, sizeof(roles));
    snprintf(roles[0].iface, sizeof(roles[0].iface), "wwan0");
    roles[0].metered = 1;
    roles[0].monthly_allowance_mb = 50000;
    roles[0].reset_day = 1;

    mqvpn_budget_mgr_t mgr;
    mqvpn_budget_mgr_init(&mgr, roles, 1);
    ASSERT_EQ_INT(mqvpn_budget_mgr_load(&mgr, "/tmp/no_such_budget_state_file"), -1,
                  "missing file -> -1 (fresh)");
    ASSERT_EQ_ULL(mgr.state[0].used_bytes, 0, "fresh state used 0");
}

int
main(void)
{
    test_init_and_find();
    test_apply_accumulates();
    test_apply_period_rollover();
    test_apply_counter_reset();
    test_non_metered_not_assessed();
    test_save_load_roundtrip();
    test_load_missing_file();

    printf("\n=== test_budget_mgr: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
