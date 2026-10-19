# Traffic shaping for mqvpn — WLB analysis and implementation plan

Status: **planning only; nothing implemented.** Written 2026-10-08 for a
two-link policy feature (unlimited-but-unreliable primary + metered secondary).
Task index: [`TASKS-traffic-shaping.md`](../TASKS-traffic-shaping.md).
Companion: [`dashboard-plan.md`](dashboard-plan.md) (web dashboard and
runtime-tunable engagement knobs).

This document is meant to be resumed cold. §2 is a self-contained description
of how the WLB scheduler works today, with source references. §3 maps the
user's requirements onto the current behaviour. §4 is the single hardest
architectural problem. §5 is the proposed design, §6–§7 the file-level plan,
§8–§9 risks/tests, §10 open questions.

---

## 1. Requirements

Let A = *unlimited but unreliable* link (the primary), B = *metered* link.

| # | Requirement | Notes |
|---|-------------|-------|
| R1 | B is used when A is too slow or contested | adaptive, not always-on |
| R2 | B adds bandwidth during a long sustained transfer | aggregation |
| R3 | Configure B's monthly data allowance; usage scales with what's left | budget-aware |
| R4 | Target servers/IPs always go through A | destination routing |
| R5 | That target traffic is **lower priority** than other traffic | QoS/priority semantics — see §10 |

---

## 2. How WLB works today

### 2.1 Where it lives and how it is selected

- Implementation: `third_party/xquic/src/transport/scheduler/xqc_scheduler_wlb.c`
  (1424 lines), header `.../xqc_scheduler_wlb.h`. This is the fork
  `mp0rta/xquic`, branch `mqvpn-main`, vendored as a submodule.
- Selected from mqvpn via `mqvpn_apply_scheduler()` in
  `src/mqvpn_conn_settings.c:24`; it assigns
  `xqc_conn_settings_t.scheduler_callback = xqc_wlb_scheduler_cb` for
  `wlb` and `wlb_udp_pin`. The callback is part of `xqc_conn_settings_t`
  (xquic.h:1088), which sits inside `xqc_conn_settings_t`.
- mqvpn config key: `[Multipath] Scheduler` (`src/config.c:586`); CLI
  `--scheduler` (`src/main.c:268`). Names are centralised in
  `src/mqvpn_sched_names.h`.
- The scheduler sees the connection and its `xqc_path_ctx_t` paths directly
  (`src/transport/xqc_multipath.h:90`), so it can read any per-path field.

### 2.2 How a packet reaches the scheduler

1. mqvpn classifies the inner IP packet with `flow_hash_pkt()`
   (`src/flow_sched.c:36`), producing a 32-bit FNV-1a 5-tuple hash:
   - inner TCP → 5-tuple hash (always pinned);
   - inner UDP → 5-tuple hash only under `wlb_udp_pin`, else the sentinel
     `MQVPN_FLOW_HASH_UNPINNED = 0xFFFFFFFF`;
   - ICMP/other/malformed → sentinel or `0`.
2. mqvpn calls `xqc_conn_set_dgram_flow_hash(conn, fh)`
   (`src/mqvpn_client.c:3903`, `src/mqvpn_server.c:2603`), then
   `xqc_h3_ext_datagram_send(...)`.
3. xquic copies it: `packet_out->po_flow_hash = conn->next_dgram_flow_hash`
   (`src/transport/xqc_packet_out.c:1339`). This is set **only** on the
   datagram write path; STREAM packets have `po_flow_hash == 0`.

So the *only* per-packet scheduling hint that crosses into xquic today is
`po_flow_hash`, and the only per-path identity the scheduler has is the path
itself.

### 2.3 Packet classes in `xqc_wlb_scheduler_get_path`

`xqc_scheduler_wlb.c:1154`. Order of dispatch:

| Condition | Placement |
|-----------|-----------|
| `reinject` (a duplicate) | MinRTT, origin path excluded (`wlb_minrtt_fallback`) |
| `po_flow_hash == 0` (STREAM + control) | MinRTT fallback — a measured choice, see the long rationale at line 1169 |
| `po_flow_hash == UNPINNED` | per-packet smooth WRR, no flow table |
| any other hash | flow-table lookup; a miss is placed by WRR and then **pinned** |

The hybrid TCP lane's stream traffic therefore always takes the MinRTT
fallback by construction (DD §7). Datagram-lane traffic is what WLB shapes.

### 2.4 Weight learning (`wlb_compute_goodput_weight`, line 522)

- `xqc_wlb_scheduler_on_app_packet_acked` (line 1033) accumulates
  `app_delivered` bytes per path; it is invoked from `xqc_send_ctl.c:1782`.
- Every ≥200 ms of wall clock, a sample is taken:
  `sample_Bps = Δdelivered / Δt`, folded into an EWMA
  (`goodput_ewma_Bps`, 7/8 decay).
- `raw weight = goodput_ewma_Bps`, then:
  - **app-limited credit**: if `xqc_send_ctl_is_app_limited(ctl)`, raise the
    weight to the CC's bandwidth estimate (`xqc_send_ctl_get_est_bw`). This
    prevents a deliberately under-fed path from being starved forever.
  - **loss haircut**: linear discount above 2% recent loss
    (`xqc_path_recent_loss_rate`).
  - **bufferbloat haircut**: if `srtt > 2 * min_rtt`, scale by
    `2*min_rtt/srtt`. Keeps a deep-queue path from poisoning reorder.
  - A warmed path whose goodput decayed to 0 is sent to weight 1.
- Warm-up: a path stays in warm-up until 3 s of activity or 1 MiB acked
  (`WLB_WARMUP_ACTIVE_US`, `WLB_WARMUP_ACK_BYTES`), and gets a 20% floor;
  steady-state floor is 5% (`WLB_STEADY_FLOOR_PCT`).

### 2.5 Normalisation and smooth WRR

- `wlb_normalize_weights` (line 676) scales raw weights into quanta summing to
  `WLB_QUANTUM_TOTAL = 100`, with the per-path floors above.
- `wlb_wrr_select` (line 911) is smooth deficit WRR: each sendable path gains
  its quantum; the highest-deficit path is chosen and subtracts the total.
- Weights refresh at round boundaries (`wlb_start_round`, line 835) and
  immediately when the schedulable path set changes
  (`wlb_active_paths_match_cache`, line 657; `force_refresh_paths`).

### 2.6 Pinning and spillover

- Inner TCP flows are pinned via a 4096-entry open-addressing table
  (`WLB_FLOW_TABLE_SIZE`), keyed by `po_flow_hash`, value = path_id.
- The pin target is chosen by a **second** smooth WRR that ignores cwnd
  (`wlb_pick_pin_path`, line 874), so a momentary cwnd block does not freeze a
  flow onto a narrow path.
- If the pinned path is cwnd-blocked, the packet is sent on another path
  **without** changing the pin ("soft pin", line 1308) — trades a little
  reordering for flow stability and loss resilience.
- Pins expire after 60 s idle, on unusable/blackholed paths, or on loss
  ≥ 2% (`wlb_flow_expire`, line 291).

### 2.7 Path liveness, eviction, recovery

- `wlb_path_schedulable` = transport OK (`ACTIVE`, not `FROZEN`, no socket
  error) **and** not blackholed (`ctl_pto_count < 3`).
- A blackholed path leaves the WRR set and receives one probe datagram per
  500 ms (`wlb_pick_evicted_probe`, line 1090) so it can recover.
- On a genuine recovery (`ever_lost_path` gate) pins are wiped and a 1 s
  "recovery grace" unpins flows (`recovery_unpin_until_us`) so they can re-pin
  to the restored path.
- Note: `STANDBY` paths are **not** excluded by WLB (only `FROZEN` is), so the
  draft-21 STANDBY status does not by itself take a path out of WLB.

### 2.8 Observability

- Internal per-path view: `xqc_wlb_path_stats_t` and
  `xqc_wlb_scheduler_copy_path_stats` (`xqc_scheduler_wlb.h:23`).
- Public path metrics flow through `xqc_conn_get_stats()` →
  `paths_info[]` → `mqvpn_path_stats_t` (`include/libmqvpn.h:476`), which has
  `bytes_tx`, `bytes_rx`, `srtt_us`, `cwnd`, `pkt_*`, `state`.
- Control API: `get_status` path objects (`docs/control-api.md` §5.5),
  `get_stats`, `get_build_info`. OMR parses these (DD §14).

### 2.9 What WLB does *not* know

- Which path is metered / has a budget.
- Any per-path role or priority.
- Anything about the inner packet except its flow hash — not destination IP,
  not a class.
- Any per-connection policy on the server: server conn settings are applied
  once per engine (`xqc_server_set_conn_settings`, `src/mqvpn_server.c:2181`),
  not per client connection.

---

## 3. Gap analysis

| Req | Current WLB | Gap |
|-----|-------------|-----|
| R1 B on when A slow | WLB already weights by measured goodput and spills off a blocked path | No *role*: it will use B whenever B measures useful, including when A is fine. No "A must be saturated first" policy. |
| R2 B for sustained transfer | WLB aggregates by weight continuously | Same as R1: B is engaged even for short/bursty traffic. Needs demand gating. |
| R3 monthly allowance | none | No byte budget, no accounting window, no persistence, no reset. |
| R4 destination routing | only a 5-tuple flow hash; the scheduler cannot see the destination | Needs a new classification + per-packet policy hint, and (for downloads) server knowledge. |
| R5 priority | none | Needs a scheduling class that WRR/pinning honours. |

A path-role/budget feature is a *policy* layer; WLB is a *measurement* layer.
They can compose, but WLB has no place to read policy from today.

---

## 4. The hard problem: which direction is being shaped?

mqvpn is multipath in both directions. The **sender** picks paths:

- Upload (client → server): the **client's** WLB instance schedules.
- Download (server → client): the **server's** WLB instance schedules, on the
  server's view of the same paths.

Consequences:

- **Upload-only shaping is client-local.** The client knows which interface is
  metered, can budget its own sends, and can gate them. No protocol change.
- **Download shaping requires the server to know the client's link policy.**
  The server's WLB sees `xqc_path_ctx_t`s but nothing marks path B as metered,
  and server conn settings are global per engine. The server would need a
  per-connection policy: either
  - **(a) a new in-band exchange** (precedent: the `mqvpn-reorder` HTTP header
    negotiation, `src/reorder.h` §19, `src/mqvpn_client.c:1930-1976`,
    `src/mqvpn_server.c:939`) carrying per-path role + remaining allowance +
    destination rules; or
  - **(b) reuse of draft-21 PATH_STATUS** (`xqc_conn_mark_path_frozen` /
    `_available`, already used at `src/mqvpn_client.c:457`) to tell the server
    to stop/start sending on B. Binary and coarse; cannot express "use B for
    30% of the remaining allowance" or "use B only for bulk".

Both (a) and (b) are compatibility-relevant: (a) is a new mqvpn extension
(unknown peers must ignore it gracefully, like `mqvpn-reorder`); (b) is
standard but changes peer-visible path status.

**Decision (Q1): both directions are in scope.** Download shaping is required,
so the server must learn the client's policy. See §5.5.

### 4.1 Deployment assumption: single client, single server (one-off)

This is a private one-off deployment: the same operator controls both ends and
there is exactly one client and one server. That removes most of the Phase-4
protocol risk:

- The policy does **not** need a versioned, multi-tenant exchange. The server
  can be configured with the same path policy, keyed by the client's metered
  WAN source address (the server already sees each path's peer address), or by
  a trivial one-shot announcement.
- Per-connection server policy is trivially the only policy — the
  global-vs-per-client split noted in §2.9 does not bite.
- The client stays authoritative for the allowance; the server only needs the
  current `remaining` value pushed periodically (or even polled).

Keep the graceful-ignore property anyway (an old server must not break), but
the scope is now "one config section on the server + a small allowance push",
not a full negotiation protocol. Open question Q8 narrows accordingly.

---

## 5. Proposed design

Guiding principle: keep WLB as the scheduling engine and feed it **policy**
that it currently lacks, rather than forking a parallel scheduler. This matches
mqvpn AGENTS.md ("WLB is the production scheduler") and DD §6.

### 5.1 Policy data model

Per-path policy (repeated section, mirroring the existing `[ReorderRule]`
precedent at `src/config.c:1009`):

```ini
[Multipath]
Scheduler = wlb
Path = eth0        ; unlimited primary
Path = wwan0       ; metered secondary

[Path]
Iface = wwan0
Metered = true
MonthlyAllowanceMB = 50000
ResetDay = 1
Priority = 50          ; optional explicit share cap when engaged
```

Destination policy (repeated section, analogous to `[ReorderRule]`):

```ini
[Route]
Dest = 203.0.113.7/32     ; or hostname resolved at config load
Action = unlimited        ; force through the unlimited path
Priority = low            ; semantics per Q2
```

Rationale for repeated sections: `parse_section` currently matches fixed names
(`src/config.c:100`), but `[ReorderRule]` already demonstrates a repeated,
stateful section (each header pushes a slot, keys fill it). `[Path]`/`[Route]`
follow that pattern without inventing a new syntax. A flat indexed form
(`Path0Metered=...`) is a fallback if the parser cost is judged too high.

Note the existing `Path =` key (config.c:998) only stores an interface name;
policy is attached by matching `Iface` to that name.

### 5.2 Path roles and budget accounting

Two layers:

- **mqvpn (CLI/platform):** owns the config, the wall-clock calendar window,
  persistence, and the *remaining allowance* number. It already has per-path
  `bytes_tx`/`bytes_rx` (`src/path_entry_internal.h:78`, surfaced in
  `mqvpn_path_stats_t`).
- **xquic WLB (fork):** receives per-path policy {metered, allowance_remaining,
  role/priority} and per-packet class; enforces them at selection time.

**Decided:** bill **bytes sent on B in both directions** (conservative — a
retransmission still costs metered data), and expose sent/acked for
observability. The client is authoritative (it owns the metered link) and
periodically reports `remaining` to the server so the server's scheduler can
shape downloads (see §5.5). Reset is on a configurable **calendar day**.

Persistence: a small state file (path + period key + used bytes) written by
the CLI, e.g. under the state dir. This is a platform concern; the library
stays sans-I/O and does not touch files (DD §1/§2). Reset by calendar day
(`ResetDay`), with the period key in the file so a restart mid-month resumes.

### 5.3 Scheduler extension inside the WLB fork

Minimal, localised additions in `xqc_scheduler_wlb.c` (and small fields in
`xqc_path_ctx_t` or the scheduler's own per-path array):

1. **Per-path policy fields** on `wlb_path_weight_t` (or read from the path
   ctx): `metered`, `allowance_remaining`, `role_priority`, `engaged`.
2. **Budget gate** in `wlb_normalize_weights` / `wlb_wrr_select`: a metered path
   with `allowance_remaining == 0` is removed from the schedulable set (but
   still probed like a blackholed path so it can return after reset). As the
   allowance approaches 0, scale its quantum down (e.g. proportional to
   remaining/total) so the last bytes are spent deliberately, not in one burst.
3. **Demand gating (R1/R2):** a metered path is only *eligible* when the
   primary is saturated. Candidate signal: the primary is **not**
   app-limited (it has a standing backlog) for some sustained interval
   (`xqc_send_ctl_is_app_limited` already used at line 575), or its goodput is
   well below its CC estimate while demand persists. Once engaged, keep it in
   the WRR set for a hysteresis interval to avoid flapping.
4. **Class/priority (R4/R5):** a per-packet class lets a packet be restricted
   to a path subset and/or given a lower WRR share. See §5.4.
5. **Failure/eviction unchanged:** the existing PTO/loss/idle eviction and
   recovery grace stay as-is; budget exhaustion is a new, separate
   "not eligible" reason that must not wipe pins in a way that breaks the
   recovery logic.

Per DD §13, any new observability (allowance remaining, engaged, class counts)
belongs in the extended metrics block of `xqc_path_metrics_t` via
`xqc_conn_get_stats()`, not a new scheduler API.

### 5.4 Destination routing and priority (R4/R5)

The scheduler cannot see the inner destination. Two ways to carry the class:

- **Option A — per-packet hint (recommended).** Add a small field to
  `xqc_packet_out_t` (e.g. `po_sched_class`) set via a new
  `xqc_conn_set_dgram_sched_class(conn, class)` analogous to
  `xqc_conn_set_dgram_flow_hash` (`xqc_conn.c:4017`,
  `xqc_packet_out.c:1339`). mqvpn computes the class in `flow_hash_pkt`'s
  caller (`src/mqvpn_client.c:3901`, `src/mqvpn_server.c:2602`) from the
  destination/source IP against the `[Route]` table. Classes, e.g.:
  `DEFAULT`, `FORCE_UNLIMITED`, `LOW_PRIORITY`.
- **Option B — encode in the flow hash namespace.** Reserve a high-bit tag in
  `po_flow_hash`. Cheaper (no new xquic field) but overloads a field whose
  meaning is "flow identity"; pinning and the flow table would need care.
  Not recommended.

Enforcement in WLB:
- `FORCE_UNLIMITED`: restrict candidate paths to the unlimited path(s). For
  upload this is enough. For download the server needs the same rule (§5.5).
- `LOW_PRIORITY` (**decided: yield on contention**): these packets are
  restricted to the unlimited path and only send when higher-priority traffic
  has no backlog. Implement as a second WRR deficit counter per class on the
  unlimited path: default-class packets accrue deficit normally, low-priority
  packets only accrue/send when the default class has nothing queued. They
  must never consume metered bytes. Combined with `FORCE_UNLIMITED`, the two
  can be one class ("target traffic") whose rule is: unlimited path only, and
  yield to general traffic on it.

Classification needs care with the existing flow hash: a pinned TCP flow whose
5-tuple matches a `[Route]` must be pinned to the unlimited path, and the pin
must be invalidated if a route is added/removed at runtime.

### 5.5 Propagating policy to the server (decided: required)

Download shaping is in scope, so the server's scheduler must honour the
client's policy. Given the one-off deployment (§4.1), prefer the simplest
mechanism that works:

1. **Preferred (one-off): server-side config keyed by the client's metered
   WAN source address.** The server already sees each path's peer address;
   matching the metered path by source IP needs no protocol at all. Only the
   *remaining allowance* is dynamic, and only that needs to reach the server.
2. **Dynamic part:** push `remaining` (and, if desired, the demand-gate state)
   to the server periodically — a small capsule/control message, or a
   PATH_STATUS freeze when it hits zero.
3. **General shape (if ever needed beyond this deployment):** a versioned
   in-band exchange modelled on `mqvpn-reorder`, ignored by older peers; server
   installs it per connection (needs a per-connection path→policy setter —
   today server settings are engine-global, §2.9).
4. **Coarse fallback:** draft-21 `PATH_STATUS` (`mark_path_frozen` /
   `available`) as an on/off budget-exhaustion gate only.

### 5.6 Control API and persistence

- `get_status`/`get_stats`: add per-path `role`, `metered`, `allowance_total`,
  `allowance_used`, `allowance_remaining`, `engaged`. Additive fields only —
  OMR parses these replies (DD §14), so never rename/remove.
- Persistence file owned by the CLI/platform; library APIs stay pure.
- A runtime control command to adjust the allowance is possible but is a new
  compatibility surface; defer unless requested.

---

## 6. File-level plan (first cut)

mqvpn side:
- `src/config.c`, `src/config.h` — `[Path]`/`[Route]` sections and storage.
- `src/mqvpn_config.c` — JSON equivalents (server/OMR config path).
- `src/vpn_client.h`, `src/main.c` — carry policy into `mqvpn_client_cfg_t`.
- `src/flow_sched.c/.h` — classification hook (or a sibling classifier).
- `src/mqvpn_client.c` — set per-path policy on path add; set per-packet class
  before `xqc_h3_ext_datagram_send`; budget accounting from path stats.
- `src/mqvpn_server.c` — per-connection policy install (Phase 4); class from
  inner source IP on the download path.
- `include/libmqvpn.h` — additive stats fields, new public setters if needed
  (mind DD §13: public functions are the irreversible direction).
- `tests/test_config.c`, `tests/test_flow_sched.c`, `tests/test_control_socket.c`.

xquic fork side (`third_party/xquic`, branch `mqvpn-main` / `mqvpn-dev`):
- `src/transport/scheduler/xqc_scheduler_wlb.c` — policy-aware weights, budget
  gate, demand gate, class enforcement.
- `src/transport/scheduler/xqc_scheduler_wlb.h` — policy/stat structs.
- `src/transport/xqc_packet_out.h` — `po_sched_class` (Option A).
- `src/transport/xqc_conn.c` / `xquic.h` — `xqc_conn_set_dgram_sched_class`,
  per-path policy setter.
- `src/transport/xqc_multipath.h` — per-path policy fields if stored there.
- `tests/unittest/xqc_wlb_test*.c` — paired happy/abnormal tests.

Submodule bump in mqvpn naming the fork PR/tag (mqvpn AGENTS git rules).

---

## 7. Sequencing

1. **Phase 0** (config/data model, no behaviour) — safe, reviewable.
2. **Phase 1** (budget accounting + WLB budget gate, upload) — delivers R3 for
   upload and the hardest scheduler plumbing.
3. **Phase 2** (demand gating, R1/R2 upload).
4. **Phase 3** (destination class + priority, R4/R5 upload).
5. **Phase 4** (server/download) — **required** (decision Q1 = both
   directions). Largest scope; carries the protocol-compatibility work. It can
   land after Phases 0–3 so the upload half is usable in the meantime, but it
   is not optional.
6. **Phase 5** (observability/docs).

Phases 0–3 + 5 are a coherent intermediate milestone (upload fully shaped);
Phase 4 completes downloads. The client→server policy exchange must be
versioned/ignored gracefully so a Phase-4 client still works against an older
server (upload shaped, download unshaped) and vice versa.

---

## 8. Risks and constraints

- **Compatibility surfaces (DD §14):** config keys, control-API fields and
  defaults reach OMR users. All additions must be additive; renaming/removing
  needs a compatibility note. `[Path]`/`[Route]` are new sections — old
  mqvpn ignores unknown sections? Verify: unknown sections currently parse to
  `SEC_NONE` and are warned/ignored, so an old binary fed a new config should
  still start. Confirm before relying on it.
- **Fork divergence (DD §12):** every line added to the fork is re-paid at
  each upstream merge. Keep to the scheduler dir + the two existing hooks.
  Prefer changes that could land upstream.
- **Don't break WLB's measured behaviour:** budget/demand gates must not
  regress the existing goodput learning, failover, or hybrid-lane MinRTT
  behaviour (DD §7). Benchmarks are the evidence.
- **Both-endpoint config:** users must configure both client and server for
  download shaping; document this clearly or it will look broken.
- **Calendar/month semantics:** timezones, reset day clamping, clock jumps,
  persistence corruption. Keep the model simple and document it.
- **Hostname routes:** resolving hostnames at config load means IP changes are
  missed; prefer IP/CIDR, document hostname limitations.
- **Per-connection policy on the server** is new: today server settings are
  engine-global (`src/mqvpn_server.c:2181`).

---

## 9. Testing obligations

- xquic unit tests: `third_party/xquic/tests/unittest/xqc_wlb_test*.c` —
  paired happy/abnormal for every production behaviour change (budget gate
  on/off, exhausted allowance, demand gate engage/disengage, class routing,
  route add/remove invalidation).
- mqvpn unit tests: `tests/test_config.c` (new keys + INI/JSON parity),
  `tests/test_flow_sched.c` (classification), control-socket reply fields.
- Netns e2e (`scripts/ci_e2e/`): shaped two-path pair; assert aggregation
  engages only under load and that the metered path stops at budget 0.
- Sanitizer build (clang `-Werror`, ASan/UBSan) before pushing non-trivial C
  (DD §10).
- Log wording used by e2e is an invariant (DD §10): update markers with any
  new log line.

---

## 10. Decisions and remaining questions

### Decided (2026-10-08, by the maintainer)

- **Q1 — Direction: both.** Uploads shaped by the client's scheduler;
  downloads by the server's scheduler, fed by a client→server policy exchange
  (§4, §5.5). Phase 4 is required.
- **Q2 — Priority: yield on contention.** Target traffic is restricted to the
  unlimited path and only sends when general traffic has no backlog; it never
  consumes metered bytes (§5.4).
- **Q3 — Budget basis: sent bytes**, calendar month with a configurable reset
  day, persisted across restarts; acked bytes also exposed for observability
  (§5.2). (This settles Q4 as well.)

### Q5 — Engagement trigger (recommended; confirm)

WLB today is throughput-centric. It uses (see §2.4): acked-goodput EWMA,
`est_bw`, the app-limited flag, recent loss, `srtt/min_rtt`, cwnd headroom and
PTO count. It does **not** use absolute latency for datagram weighting;
latency enters only as the bufferbloat ratio and the MinRTT fallback.

Recommended trigger for "A too slow/contested", built only from existing
signals:

- **Engage B** when A is **not app-limited** for ~1 s (standing backlog =
  offered load exceeds A's capacity), **or** A's goodput `< f × est_bw`, **or**
  A's recent loss spikes.
- **Disengage B** after ~2–5 s of A being app-limited again (hysteresis, to
  avoid flapping).
- The app-limited flag (`xqc_send_ctl_is_app_limited`) is the key
  discriminator: app-limited = under-fed (don't spend metered bytes);
  not app-limited = saturated/contested (spend them). This covers R1 and R2
  together, and is the same signal WLB already trusts.
- New knobs: the fraction `f` and the two durations; optionally a loss
  threshold. Confirm defaults.
- These knobs are intended to be **runtime-tunable** from the web dashboard
  (see [`dashboard-plan.md`](dashboard-plan.md) §6): config supplies defaults,
  the dashboard overrides them live, and the override persists.

### Still open

- **Q6 — Config syntax:** repeated `[Path]`/`[Route]` sections (recommended) vs
  flat indexed keys.
- **Q7 — Scope of destination matching:** IP/CIDR only, or hostnames too?
  (Both directions now, by Q1.)
- **Q8 — Dynamic allowance push (Phase 4):** narrowed by §4.1 — server policy
  is configured statically by source address; only `remaining` is dynamic.
  Choose the push mechanism (capsule/control message vs PATH_STATUS at zero).
- **Q9 — Server-side policy storage:** trivial under §4.1 (one connection);
  confirm the metered path is matched by peer source address vs another key.

---

## Appendix A — key source references

| Topic | Location |
|-------|----------|
| WLB algorithm + rationale | `third_party/xquic/src/transport/scheduler/xqc_scheduler_wlb.c` (header comment L1-86) |
| WLB dispatch / classes | same file, `xqc_wlb_scheduler_get_path` L1154 |
| Weight learning | same file, `wlb_compute_goodput_weight` L522; ack hook L1033 |
| Normalise + WRR | same file, `wlb_normalize_weights` L676, `wlb_wrr_select` L911 |
| Pinning | same file, `wlb_pick_pin_path` L874, flow table L199-269 |
| Eviction/recovery/probe | same file, `wlb_flow_expire` L291, `wlb_pick_evicted_probe` L1090 |
| Per-path stats struct | `xqc_scheduler_wlb.h:23`; `include/libmqvpn.h:476` |
| Scheduler selection | `src/mqvpn_conn_settings.c:24` |
| Flow hash | `src/flow_sched.c:36`, `src/flow_sched.h` |
| Flow hash → xquic | `src/mqvpn_client.c:3901-3905`, `src/mqvpn_server.c:2602`; `xqc_conn.c:4017`; `xqc_packet_out.c:1339` |
| Path ctx fields | `third_party/xquic/src/transport/xqc_multipath.h:90` |
| PATH_STATUS helpers | `xqc_conn_mark_path_{standby,available,frozen}` (xquic.h:2529-2551); used at `src/mqvpn_client.c:457` |
| In-band negotiation precedent | `src/reorder.h` §19; `src/mqvpn_client.c:1930-1976`; `src/mqvpn_server.c:939` |
| Config parser / repeated sections | `src/config.c:100` (sections), `:998` (Path), `:1009` (ReorderRule) |
| Config-key rule | mqvpn AGENTS.md / DD §10 |
| Scheduler design rules | DD §6, §7, §12, §13, §14 |
| xquic dev pipeline | `third_party/xquic/harness/pipelines/dev-pipeline.md` |
