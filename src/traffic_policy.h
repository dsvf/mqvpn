// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * traffic_policy.h — shared traffic-shaping policy types (xquic-free).
 *
 * Split out of config.h so the CLI runtime (vpn_client.h) and the CLI-side
 * budget manager (budget_mgr.h) can use the same per-path role and route
 * shapes without pulling the whole INI/JSON config parser.
 */
#ifndef MQVPN_TRAFFIC_POLICY_H
#define MQVPN_TRAFFIC_POLICY_H

#include <stdint.h>

#include "libmqvpn.h"          /* MQVPN_MAX_PATHS */
#include "hybrid/classifier.h" /* mqvpn_cidr_entry_t */

#define MQVPN_CONFIG_MAX_PATH_ROLES MQVPN_MAX_PATHS
#define MQVPN_CONFIG_MAX_ROUTES 64

/* One metered-path budget role, keyed by interface name. An interface with
 * no entry is unlimited (aggregate freely, i.e. today's WLB behaviour). */
typedef struct {
    char iface[32]; /* matches paths[][32] / backup_paths[][32] */
    int  metered;   /* 1 = budgeted path (spend allowance deliberately) */
    unsigned long long monthly_allowance_mb; /* 0 = no budget */
    int  reset_day; /* calendar day of month the allowance resets, 1..31 */
    int  priority;  /* optional engaged-share cap 0..100; 0 = auto (WLB) */
} mqvpn_path_role_t;

typedef enum {
    MQVPN_ROUTE_ACTION_UNLIMITED = 0, /* force traffic to the unlimited path(s) */
} mqvpn_route_action_t;

typedef enum {
    MQVPN_ROUTE_PRIORITY_NORMAL = 0,
    MQVPN_ROUTE_PRIORITY_LOW = 1, /* yield to general traffic (background) */
} mqvpn_route_priority_t;

typedef struct {
    mqvpn_cidr_entry_t cidr; /* parsed Dest (IP/CIDR) */
    uint8_t action;          /* mqvpn_route_action_t */
    uint8_t priority;        /* mqvpn_route_priority_t */
} mqvpn_route_cfg_t;

#endif /* MQVPN_TRAFFIC_POLICY_H */
