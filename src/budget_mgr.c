// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

#include "budget_mgr.h"

#include <stdio.h>
#include <string.h>

void
mqvpn_budget_mgr_init(mqvpn_budget_mgr_t *mgr, const mqvpn_path_role_t *roles,
                      int n_roles)
{
    memset(mgr, 0, sizeof(*mgr));
    if (n_roles > MQVPN_BUDGET_MGR_MAX_PATHS) n_roles = MQVPN_BUDGET_MGR_MAX_PATHS;
    if (roles && n_roles > 0) {
        memcpy(mgr->roles, roles, (size_t)n_roles * sizeof(mgr->roles[0]));
        mgr->n_roles = n_roles;
    }
}

int
mqvpn_budget_mgr_find(const mqvpn_budget_mgr_t *mgr, const char *iface)
{
    if (!mgr || !iface) return -1;
    for (int i = 0; i < mgr->n_roles; i++) {
        if (mgr->roles[i].iface[0] && strcmp(mgr->roles[i].iface, iface) == 0)
            return i;
    }
    return -1;
}

static void
assess_role(const mqvpn_path_role_t *r, const mqvpn_budget_state_t *st,
            const mqvpn_budget_date_t *now, mqvpn_budget_assessment_t *out)
{
    uint64_t allowance = mqvpn_budget_mb_to_bytes(r->monthly_allowance_mb);
    mqvpn_budget_assess(allowance, r->reset_day, st, now, out);
}

void
mqvpn_budget_mgr_assess(const mqvpn_budget_mgr_t *mgr, const char *iface,
                        const mqvpn_budget_date_t *now,
                        mqvpn_budget_assessment_t *out)
{
    memset(out, 0, sizeof(*out));
    int idx = mqvpn_budget_mgr_find(mgr, iface);
    if (idx < 0 || !mgr->roles[idx].metered) return;
    assess_role(&mgr->roles[idx], &mgr->state[idx], now, out);
}

void
mqvpn_budget_mgr_apply(mqvpn_budget_mgr_t *mgr, const char *iface,
                       uint64_t bytes_tx, const mqvpn_budget_date_t *now,
                       mqvpn_budget_assessment_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!now) return;
    int idx = mqvpn_budget_mgr_find(mgr, iface);
    if (idx < 0 || !mgr->roles[idx].metered) return;

    uint64_t period_key = mqvpn_budget_period_key(mgr->roles[idx].reset_day, now);
    if (mgr->state[idx].period != period_key) {
        int first_billing = (mgr->state[idx].period == 0);
        mgr->state[idx].period = period_key;
        mgr->state[idx].used_bytes = 0;
        if (!first_billing) {
            /* Period rollover mid-session: bill only bytes sent from now on;
             * the pre-midnight tick interval is lost to the old period. */
            mgr->last_seen_tx[idx] = bytes_tx;
        }
    }
    if (bytes_tx >= mgr->last_seen_tx[idx]) {
        mgr->state[idx].used_bytes += bytes_tx - mgr->last_seen_tx[idx];
    }
    mgr->last_seen_tx[idx] = bytes_tx;

    assess_role(&mgr->roles[idx], &mgr->state[idx], now, out);
}

int
mqvpn_budget_mgr_save(const mqvpn_budget_mgr_t *mgr, const char *path)
{
    if (!mgr || !path || !path[0]) return -1;

    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    int rc = 0;
    for (int i = 0; i < mgr->n_roles; i++) {
        if (!mgr->roles[i].metered) continue;
        if (fprintf(f, "%s %llu %llu\n", mgr->roles[i].iface,
                    (unsigned long long)mgr->state[i].period,
                    (unsigned long long)mgr->state[i].used_bytes) < 0) {
            rc = -1;
            break;
        }
    }
    if (fclose(f) != 0) rc = -1;
    if (rc == 0 && rename(tmp, path) != 0) rc = -1;
    if (rc != 0) remove(tmp);
    return rc;
}

int
mqvpn_budget_mgr_load(mqvpn_budget_mgr_t *mgr, const char *path)
{
    if (!mgr || !path || !path[0]) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1; /* missing file == fresh states; caller logs */

    char line[128];
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        unsigned long long period = 0, used = 0;
        if (sscanf(line, "%31s %llu %llu", iface, &period, &used) == 3) {
            int idx = mqvpn_budget_mgr_find(mgr, iface);
            if (idx >= 0 && mgr->roles[idx].metered) {
                mgr->state[idx].period = (uint64_t)period;
                mgr->state[idx].used_bytes = (uint64_t)used;
                mgr->last_seen_tx[idx] = 0; /* fresh session baseline */
            }
        }
    }
    if (ferror(f)) rc = -1;
    fclose(f);
    return rc;
}
