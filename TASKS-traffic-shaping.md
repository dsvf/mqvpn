# Tasks — metered-path traffic shaping for mqvpn (fork)

Status: **Phase 0 done and committed; P1–P6 planned, no code yet.**

This file is the resume index for a clean session. It records the goal, the
locked decisions, what the fork already provides, the data model added in
Phase 0, the remaining phases, and how to build/test. The WLB internals and
the original design rationale are in the upstream repo
(`mqvpn/docs/traffic-shaping-plan.md`, `mqvpn/docs/dashboard-plan.md`) — read
those for the *why*; this file is the fork-specific *what/where*.

Repo: `mqvpn_weighed` (a.k.a. Ysurac/mqvpn, "fork of official mqvpn with path
weight support"). Branch: `feat/metered-path-shaping` (off `dev`).

---

## 1. Goal (user's words, restated)

Three mobile/4G links on a Linux client, aggregating to one mqvpn server the
operator also controls. Exactly one client, one server.

- **Link 1** = unlimited but possibly slow/congested → primary, carry almost
  everything.
- **Link 2** = large but finite monthly allowance → used only to (a) boost
  speed when link 1 is "slow"/congested, or (b) opportunistically to use up
  data that would otherwise expire at month end.
- **Link 3** = small allowance → failover only (when 1 AND 2 are down), also
  budgeted.
- **Destination policy** — certain destinations (e.g. a backup server) always
  go through link 1 (the unlimited link), at lower priority (background, may
  retry, must not consume metered data).
- **Download direction** is in scope (server-side shaping too).
- "Legacy" compatibility with the unmodified Android app is nice-to-have.

## 2. Locked decisions (2026-10)

| Q | Decision |
|---|----------|
| Client platform | Linux, modifiable (owns the SIMs, does upload shaping + budget authority) |
| Base tree | Extend `mqvpn_weighed` (Ysurac fork), not upstream `mqvpn` |
| Budget vs failover | The "slow" trigger overrides the monthly ramp — true congestion still spends link 2 until allowance is truly exhausted; the ramp gates only the *general speed-boost* usage |
| Link 3 | Failover-only (never speed-boost) **and** budgeted |
| Direction | Both (client shapes upload, server shapes download) |
| "Slow" trigger | Plan Q5: link 1 not app-limited for ~1 s (standing backlog) OR goodput < f × `est_bw` OR loss spike; disengage after a hysteresis window of app-limited time |

## 3. What the fork already gives us (reuse, don't rebuild)

| Need | Fork mechanism |
|------|----------------|
| client↔server path-policy transport | `PATH_LABEL` capsule + `xqc_conn_set_path_weight`/`_dscp_mask` (`src/mqvpn_path_label.c`, `mqvpn_path_label.h`); already syncs weight/dscp both ways |
| link-3 failover-only | `MQVPN_PATH_FLAG_BACKUP` + `[Multipath] BackupPath` + `backup`/`backup_fec` schedulers |
| runtime weight control | `set_path_weight`/`set_path_dscp_mask` control API (client **and** server mode) |
| per-(user,iface) weight persistence | `path_policy` JSON array + `mqvpn_path_policy_t` |
| config parser w/ repeated sections + JSON arrays | `src/config.c` (INI + JSON via one descriptor table + hand-coded complex keys) |

## 4. What still has to be built (the gaps)

1. **Role + budget model** — `metered`, `monthly allowance`, `reset day`,
   persistence, and the linear monthly ramp. (Phase 0 added the *config*; the
   *accounting/persistence* is P1.)
2. **Demand gate** (the "magic") — in WLB: engage link 2 when link 1 is
   saturated per Q5; hysteresis on disengage. Uses signals WLB already
   computes (`xqc_send_ctl_is_app_limited`, goodput EWMA, `est_bw`, loss).
3. **Destination routing** — classify inner destination IP → per-packet class
   → scheduler restricts to the unlimited path + yield-on-contention.
4. **Server/download enforcement** — propagate role/budget/remaining/class to
   the server; reuse/extend `PATH_LABEL`.
5. **Budgeted link 3** — the `backup` path is currently pure failover with no
   allowance counter.

## 5. Data model added in Phase 0 (parse/validate/log only)

`src/config.h`:

- `mqvpn_path_role_t { iface[32]; int metered; unsigned long long
  monthly_allowance_mb; int reset_day; int priority; }`
- `mqvpn_route_action_t { MQVPN_ROUTE_ACTION_UNLIMITED }`
- `mqvpn_route_priority_t { NORMAL, LOW }`
- `mqvpn_route_cfg_t { mqvpn_cidr_entry_t cidr; uint8_t action; uint8_t
  priority; }`
- `mqvpn_file_config_t` grows by: `path_roles[]` + `n_path_roles`
  (`MQVPN_CONFIG_MAX_PATH_ROLES` == `MQVPN_CONFIG_MAX_PATHS`), `routes[]` +
  `n_routes` (`MQVPN_CONFIG_MAX_ROUTES == 64`).

INI surface (client config):

```ini
[Multipath]
Scheduler = wlb
Path = wwan0        ; link 1: unlimited (no [Path] section → unlimited)
Path = wwan1        ; link 2: metered
BackupPath = wwan2  ; link 3: failover-only, budgeted (later)

[Path]
Iface = wwan1
Metered = true
MonthlyAllowanceMB = 50000
ResetDay = 1
Priority = 0

[Route]
Dest = 203.0.113.7/32
Action = unlimited
Priority = low
```

JSON surface (same data):

```json
{
  "path_roles": [
    {"iface":"wwan1","metered":true,"monthly_allowance_mb":50000,"reset_day":1,"priority":0}
  ],
  "routes": [
    {"dest":"203.0.113.7/32","action":"unlimited","priority":"low"}
  ]
}
```

`[Path]`/`[Route]` are top-level sections (new `SEC_PATH`/`SEC_ROUTE` in
`src/config.c`); `path_roles`/`routes` are JSON-only arrays. Values are
validated (bool/uint/int ranges, CIDR syntax) and invalid values warn-and-
keep-default; unknown sections/keys already warn-and-continue, so an older
binary still starts on a new config.

## 6. Phase plan

Phases are ordered by risk. Do not start a later phase before the earlier
one's acceptance criteria and verification are green.

### P0 — config/data model (DONE, this branch)
- [x] `[Path]`/`[Route]` INI sections + `path_roles`/`routes` JSON arrays.
- [x] Validate + log; no behavioural change.
- [x] `tests/test_config.c` cases (677 pass incl. 48 new).
- [x] `website/guide/configuration.md` (+ `ja/`) documented.

### P1 — budget accounting + persistence (DONE)
- **P1a — pure accounting core:** `src/mqvpn_budget.h/.c` (xquic-free): period
  key/days math (ResetDay clamped to month length), the linear ramp
  (`used < 70% × (day_of_period/period_days × allowance)` → speed-boost
  allowed), exhaustion, and the persisted-state wire format. `tests/test_budget.c`.
- **P1b — CLI manager + persistence + control API:**
  - `src/budget_mgr.h/.c` (CLI-side, does file I/O): owns the roles + per-path
    state, folds a live `bytes_tx` snapshot into `used_bytes` (handling period
    rollover and path-counter resets), persists to `<BudgetStateFile>` (atomic
    write). `tests/test_budget_mgr.c`.
  - `src/traffic_policy.h` — shared `mqvpn_path_role_t`/`mqvpn_route_cfg_t`
    (moved out of `config.h` so `vpn_client.h` and the runtime share them).
  - `[Multipath] BudgetStateFile` config key (INI + JSON) carries the state
    path into `mqvpn_client_cfg_t` (`src/vpn_client.h`, `src/main.c`).
  - `platform_linux.c` creates a `mqvpn_budget_mgr_t` on the run loop, loads
    state at startup, folds `mqvpn_client_get_paths()` bytes_tx every
    `STATUS_INTERVAL_SEC` (30 s), saves, and flushes on shutdown.
  - `control_socket.c` `get_status` (client mode) now emits a `"budget":[...]`
    array (`iface`, `allowance_mb`, `used_bytes`, `remaining_bytes`,
    `exhausted`, `speed_boost_allowed`). Server mode shape unchanged (OMR
    compat).
- The Q5 slow-trigger override of the ramp is a scheduler concern (P2): the
  budget module reports `exhausted` (hard stop) and `speed_boost_allowed`
  (ramp); the scheduler combines `!exhausted && (speed_boost_allowed || slow)`.
- Verification: full Debug/Release build green; `ctest` 60/60; new tests
  `test_budget` (42) + `test_budget_mgr` (23); `test_config` 678.

### P2 — WLB policy layer in the xquic fork (upload)
- Per-path policy fields (`metered`, `allowance_remaining`, `role_priority`,
  `engaged`) reachable by WLB.
- Budget gate in `wlb_normalize_weights`/`wlb_wrr_select`: exhausted metered
  path leaves the schedulable set (probed like blackholed, returns on reset);
  scale its quantum as allowance nears 0.
- Demand gate per Q5: engage when primary not app-limited ~1 s / goodput <
  f×`est_bw` / loss spike; disengage after hysteresis.
- Files: `third_party/xquic/src/transport/scheduler/xqc_scheduler_wlb.c/.h`
  (+ small fields in `xqc_path_ctx_t` or the scheduler's per-path array).
  Keep divergence confined to the scheduler dir (DD §12).
- Tests: `third_party/xquic/tests/unittest/xqc_wlb_test*.c`.

### P3 — destination routing + priority (upload)
- Classify inner dst/src IP in `src/flow_sched.c` against `routes[]`; carry a
  per-packet class to the scheduler (new `po_sched_class`, set like
  `po_flow_hash` in `src/mqvpn_client.c` → `xqc_conn_set_dgram_sched_class`).
- WLB enforces `FORCE_UNLIMITED` (restrict candidate paths) + `LOW_PRIORITY`
  (second deficit counter; only send when default class has no backlog; never
  metered bytes). Invalidating pins on runtime route changes.

### P4 — server/download enforcement + allowance push
- Reuse/extend `PATH_LABEL` (or a sibling capsule) to carry role/budget/
  remaining/class client→server; server applies per-connection policy (today
  server conn settings are engine-global — `src/mqvpn_server.c:2181` upstream).
- One-off deployment ⇒ server policy can be configured statically by the
  client's metered WAN source address; only `remaining` is dynamic.
- Keep the graceful-ignore property so an older server/app is unaffected.

### P5 — budgeted link 3
- Extend the `backup` path with an allowance counter so it also stops when its
  small allowance is exhausted (currently pure failover).

### P6 — observability + docs
- `get_status`/`get_stats` per-path role/metered/allowance/engaged (additive,
  OMR parses these — never rename/remove).
- `docs/control-api.md`, website EN/ja already updated in P0 for config.

### Optional follow-on — web dashboard (runtime-tunable knobs)
Full plan lives upstream in `mqvpn/docs/dashboard-plan.md`. Fork already has a
client control socket (`set_path_weight` works in client mode), so D1 is partly
done. D2 (`set_policy` + runtime scheduler params) is the meaningful next bit.

## 7. Destination routing: native vs kernel-bypass (user's note)

The user offered a hardcoded Linux-kernel bypass (`ip route` a dest IP out the
unlimited SIM's interface) as a stopgap. Trade-off:

- **Native (P3)** — classify inside the tunnel; both directions; traffic stays
  VPN-protected; no protocol hacks. This is the plan.
- **Kernel bypass** — a policy route on the client sends the dest IP *outside*
  the tunnel on the unlimited interface. Solves both directions for free (the
  reply returns on the same physical path), zero mqvpn changes. Costs: that
  traffic is **not** tunneled/VPN'd, and it only covers the *upload*
  (client→dest) origin — it cannot steer the *download* of tunneled traffic,
  because the server still picks the path for anything it sends back.

Recommendation: native for the real fix; the kernel bypass is a legitimate
interim for the backup-server case specifically (background, retriable, and
the user is OK with it being untunneled).

## 8. Build & test

Phase 0 needs no submodule (config parser is self-contained):

```bash
cd mqvpn_weighed
cc -o /tmp/test_config tests/test_config.c src/config.c src/log.c -Isrc -Iinclude
/tmp/test_config          # expect "... 677 passed, 0 failed"
# -Werror check of the touched TU:
cc -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -c src/config.c -o /dev/null -Isrc -Iinclude
```

Full build (needed from P2 onward; xquic fork is a submodule):

```bash
git submodule update --init --recursive   # pulls Ysurac/xquic (mqvpn-omr-main) + boringssl
./build.sh                                 # Release; for tests use Debug/sanitizer per AGENTS.md
```

Claim test results only from a Debug/sanitizer build (AGENTS.md); the CI
`sanitizer` job is clang `-Werror` + ASan/UBSan. Netns e2e lives in
`scripts/ci_e2e/`.

## 9. How to resume

1. `cd mqvpn_weighed && git checkout feat/metered-path-shaping`.
2. Re-read this file, then upstream `mqvpn/docs/traffic-shaping-plan.md` §2
   (WLB internals) and §10 (Q5 trigger) as needed.
3. Start **P1** (`src/config.h`/`src/config.c` already carry the model; add
   accounting + persistence + control-API fields).
4. Keep the fork-divergence rule: P2 xquic changes stay in
   `third_party/xquic/src/transport/scheduler/` (DD §12).
5. Every config/control-API/website change must carry its EN+`ja/` mirror and
   its `tests/test_config.c` case (AGENTS.md DD §10).

## 10. Rules that constrain this work (do not re-litigate)

From `mqvpn_weighed/AGENTS.md`: WLB is the production scheduler (extend, don't
replace); scheduler observability goes in `xqc_path_metrics_t`'s extended
block; xquic fork divergence is repaid at every merge (confine it); config
keys/control-API/defaults are OMR compatibility surfaces (additive only);
log-wording changes update e2e markers.
