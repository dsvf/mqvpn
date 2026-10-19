// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * budget_mgr.h — CLI-side metered-path budget manager.
 *
 * Owns the per-path budget roles (from [Path]/"path_roles") and the persisted
 * per-path state; folds a live per-path bytes_tx snapshot into that state and
 * computes the two gates (exhausted / speed_boost_allowed) via mqvpn_budget.h.
 *
 * This module DOES file I/O: it is part of the CLI/platform, not the
 * sans-I/O libmqvpn core (DD §1/§2). The pure calendar/ramp math stays in
 * mqvpn_budget.h; this module only owns roles, state, and persistence.
 */
#ifndef MQVPN_BUDGET_MGR_H
#define MQVPN_BUDGET_MGR_H

#include "mqvpn_budget.h"
#include "traffic_policy.h"

/* One role per path; MQVPN_CONFIG_MAX_PATH_ROLES == MQVPN_MAX_PATHS. */
#define MQVPN_BUDGET_MGR_MAX_PATHS MQVPN_CONFIG_MAX_PATH_ROLES

typedef struct mqvpn_budget_mgr_s {
    mqvpn_path_role_t roles[MQVPN_BUDGET_MGR_MAX_PATHS];
    int n_roles;
    mqvpn_budget_state_t state[MQVPN_BUDGET_MGR_MAX_PATHS];
    uint64_t last_seen_tx[MQVPN_BUDGET_MGR_MAX_PATHS]; /* bytes_tx at last commit */
} mqvpn_budget_mgr_t;

/* Copy `roles` (up to MQVPN_BUDGET_MGR_MAX_PATHS) and reset all state. */
void mqvpn_budget_mgr_init(mqvpn_budget_mgr_t *mgr, const mqvpn_path_role_t *roles,
                           int n_roles);

/* Load persisted states from `path` (one "<iface> <period> <used>" line per
 * metered role). A missing file is NOT an error (fresh states); returns -1
 * only on a read error. Unknown ifaces / non-metered roles are ignored. */
int mqvpn_budget_mgr_load(mqvpn_budget_mgr_t *mgr, const char *path);

/* Persist all metered roles' states to `path` (best-effort atomic: write to
 * "<path>.tmp" then rename). Returns 0 on success, -1 on any I/O error. */
int mqvpn_budget_mgr_save(const mqvpn_budget_mgr_t *mgr, const char *path);

/* Index of the role whose iface matches `iface`, or -1. */
int mqvpn_budget_mgr_find(const mqvpn_budget_mgr_t *mgr, const char *iface);

/* Fold a live bytes_tx snapshot for `iface` into the budget state (handling
 * period rollover and path-counter resets) and compute the assessment.
 * Non-metered / unknown ifaces leave *out zeroed. */
void mqvpn_budget_mgr_apply(mqvpn_budget_mgr_t *mgr, const char *iface,
                            uint64_t bytes_tx, const mqvpn_budget_date_t *now,
                            mqvpn_budget_assessment_t *out);

/* Read-only assessment of `iface`'s current budget state (no bytes_tx fold).
 * Non-metered / unknown ifaces leave *out zeroed. */
void mqvpn_budget_mgr_assess(const mqvpn_budget_mgr_t *mgr, const char *iface,
                             const mqvpn_budget_date_t *now,
                             mqvpn_budget_assessment_t *out);

#endif /* MQVPN_BUDGET_MGR_H */
