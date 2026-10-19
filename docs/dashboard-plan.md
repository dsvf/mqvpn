# Web dashboard and runtime-configurable policy for mqvpn

Status: **planning only; nothing implemented.** Written 2026-10-08 as a
follow-on step to [`traffic-shaping-plan.md`](traffic-shaping-plan.md). Task
index: [`TASKS-traffic-shaping.md`](../TASKS-traffic-shaping.md).

The dashboard is the control plane for the metered-path feature: it shows live
and cumulative per-path usage, and it is where the engagement/disengagement
thresholds and timers are tuned without editing files or restarting.

---

## 1. Goals

1. **Live statistics**: per-path goodput, weight/share, SRTT, loss, cwnd,
   engagement state, which link is carrying what.
2. **Cumulative usage**: bytes per path this period, metered used/remaining
   against the monthly allowance, and the reset date.
3. **Distribution**: how the traffic is split across paths over time.
4. **Runtime tuning**: engagement/disengagement thresholds and timers (from
   traffic-shaping-plan §10 Q5) editable in the dashboard, applied to the
   running scheduler, and persisted.

---

## 2. What exists today

- **Control API is server-only, Linux-only.** `src/platform/linux/control_socket.c`
  (+ `.h`) is a libevent-driven loopback TCP listener that reads one JSON
  object per connection and writes one JSON reply, then closes. It is created
  from `src/platform/linux/platform_linux.c:1264` only in the server run path
  (`mqvpn_server_t`). The command table is at `control_socket.c:472`:
  `add_user`, `remove_user`, `list_users`, `get_stats`, `get_status`,
  `get_build_info`, `get_fec_stats`, `get_all_fec_stats`, `get_reorder_stats`.
- **No client control API.** `mqvpn_client_cfg_t` (`src/vpn_client.h`) has no
  control fields; the client never opens a control listener.
- **The CLI can only read.** `run_status()` (`src/platform/posix/status.c:350`,
  invoked from `src/main.c:369`) connects to the server control API, sends
  `get_status`, and prints formatted text. There is no setter of any kind.
- **No runtime settings path in xquic.** Connection settings are fixed at
  connection creation; the server applies one `xqc_conn_settings_t` to the whole
  engine (`xqc_server_set_conn_settings`, `xquic.h:2060`). There is no
  per-connection runtime update. The scheduler receives its parameters once at
  `xqc_scheduler_init` via `xqc_scheduler_params_t` (`xquic.h:951`).
- **The control API is an OMR compatibility surface** (DD §14): commands and
  reply fields are parsed by OMR. Additions are safe; renames/removals are not.
- **Both client and server run a libevent loop** (DD §1), so a control/dashboard
  listener can share the tick thread with no locking.

Consequences: the dashboard needs (a) a **client-side** telemetry/control
surface, and (b) a **runtime** path into the scheduler.

---

## 3. Architecture

### 3.1 Recommendation: embed a small HTTP front-end, share one dispatcher

Keep the policy authority on the **client** (it owns the metered link, the
budget and the trigger). Add:

1. A **client control socket**, mirroring the server's, bound to the client
   state, on the same libevent loop.
2. An **HTTP + Server-Sent-Events front-end** on the same loop, serving a
   single-page dashboard and a JSON API.
3. A **shared command dispatcher**: factor the JSON-command handling out of
   `control_socket.c` into a transport-agnostic
   `ctrl_dispatch(ctx, req, resp, resp_len)` used by both the raw TCP control
   socket and the HTTP handler, so the two cannot drift.

Rejected alternatives:

- **External dashboard daemon** polling the control API: less invasive, but the
  user asked for an integrated dashboard, it still needs a new client control
  API, and it doubles the moving parts.
- **Static HTML served by nginx** calling the raw control socket directly: the
  protocol is one-request-per-connection with no CORS/keep-alive and is
  loopback-only; it would need the same server changes anyway.

### 3.2 Why the client, not the server

The metered link, the allowance and the trigger knobs all live on the client.
The server can expose the download-direction view later, but the authoritative
dashboard is client-side. (The server already has a control API if a
server-side view is wanted.)

---

## 4. Client control API (new)

A new `src/platform/linux/client_control_socket.c/.h` modelled on the server's,
created from the client run path in `platform_linux.c` (around the `ctx.eb`
setup, `platform_linux.c:544`). Commands (additive; the client surface is new,
so no existing consumers):

| Command | Direction | Purpose |
|---------|-----------|---------|
| `get_status` | read | per-path live stats + policy state |
| `get_stats` | read | aggregate counters |
| `get_policy` | read | current trigger knobs and allowance |
| `set_policy` | write | update trigger knobs / allowance at runtime |
| `get_build_info` | read | version, scheduler |

`set_policy` must run on the tick thread. The control listener is on the same
libevent loop as the client, so a command handler can call straight into the
client/scheduler without locks (DD §1/§2).

---

## 5. HTTP + dashboard front-end

- A minimal HTTP/1.1 server on the client's libevent loop
  (`src/platform/linux/http_dashboard.c`): `GET /` (static page),
  `GET /api/status`, `GET /api/policy`, `POST /api/policy`,
  `GET /api/events` (SSE), plus a fallback poll.
- The dashboard asset is a single HTML+JS file (no build chain), either served
  from a configured directory or embedded as a C string. Embedding avoids a
  runtime path dependency; serving from disk is easier to iterate. Recommend
  embedded for release, with a `DashboardDir` override for development.
- **Real-time** via SSE (`/api/events`, 1 Hz) with a polling fallback
  (`/api/status`) for browsers/proxies that break SSE.
- The browser accumulates live samples for the distribution graph; the server
  supplies cumulative totals from its counters/store. Optionally keep a small
  server-side ring buffer (e.g. 1 h at 1 Hz) so a page reload keeps history.

---

## 6. Runtime-mutable engagement knobs

The knobs (traffic-shaping-plan §10 Q5):

| Knob | Meaning | Suggested default |
|------|---------|-------------------|
| `engage_goodput_frac_pct` | engage when primary goodput < f × est_bw | 50 |
| `engage_backlog_ms` | primary not app-limited for this long | 1000 |
| `disengage_idle_ms` | primary app-limited this long → disengage | 3000 |
| `loss_threshold_pct` | loss that also triggers engagement | 5 |
| `hysteresis_ms` | minimum time in a state before switching | 2000 |
| `allowance_total_mb` / `allowance_used_mb` / `reset_day` | budget | — |

Flow:

1. Defaults come from config (traffic-shaping-plan §5.1).
2. `set_policy` validates ranges, applies to the running scheduler, and updates
   the persisted state file so it survives restart.
3. **xquic mechanism**: add a runtime parameter path. Options, in order of
   preference:
   - **(a) Generic scheduler update callback.** Extend
     `xqc_scheduler_callback_t` with an optional
     `xqc_scheduler_set_params(void *scheduler, const xqc_scheduler_params_t *p)`
     and expose `xqc_conn_set_scheduler_params(conn, p)`. WLB stores the knobs
     in its own state. Keeps WLB-specific API out of the public surface, and
     reuses the existing params struct (grow it — ABI break is cheap in the
     fork, DD §13).
   - **(b) Per-path policy setter** for the metered/role/budget fields, if they
     are stored on `xqc_path_ctx_t`.
   - **(c) WLB-specific exported setter**: simplest but pins a scheduler-specific
     symbol; least preferred (DD §13).
4. Because this is a one-off with one connection (traffic-shaping-plan §4.1),
   there is exactly one scheduler instance to update.

**Server side (download direction)**: if the server's scheduler also needs live
knobs, the same `xqc_conn_set_scheduler_params` is driven by the client pushing
the values (traffic-shaping-plan §5.5). With one client, this is a single
connection update.

---

## 7. Data model

Live per path (from `xqc_conn_get_stats` / `mqvpn_path_stats_t` +
scheduler state):
`path_id, iface, role, metered, srtt_ms, min_rtt_ms, cwnd, in_flight,
bytes_tx, bytes_rx, pkt_sent, pkt_recv, pkt_lost, goodput_Bps, weight_pct,
warmup, engaged, allowance_remaining`.

Cumulative:
`bytes_tx/rx per path (session)`, `metered_bytes_period`, `allowance_total`,
`allowance_remaining`, `period_start`, `reset_day`.

Distribution: per-path share = `Δbytes / Δt`; the dashboard plots a stacked
area / donut. Sampling at 1 Hz is enough and cheap.

Budget accounting basis is **sent bytes, calendar month** (decided,
traffic-shaping-plan §10), persisted by the CLI/platform (library stays
sans-I/O).

---

## 8. Persistence

- One state file for the allowance window and the last-known policy overrides:
  period key, used bytes, knob values. Written on change and periodically;
  read at startup.
- Location: a state directory owned by the CLI (not the library). Follow the
  existing CLI file conventions.
- Corruption/clock-jump handling: treat an unreadable file as "start of
  period", clamp `reset_day` to the month length.

---

## 9. Security

- The existing control API is **loopback-only by default** and that must stay
  the default. A web dashboard implies remote access; do **not** silently bind
  it to `0.0.0.0`.
- Recommended: bind loopback and expose through a reverse proxy (TLS + auth),
  or add a bearer token / basic auth on the HTTP front-end.
- `set_policy` is a write path; require auth whenever the bind address is not
  loopback. Log every policy change (who/what/when) — it is an operational
  action.
- The dashboard serves no inner traffic data, only counters, so the exposure is
  metadata. Still, treat it as a control plane.

---

## 10. File-level plan

New (Linux first):
- `src/platform/linux/client_control_socket.c/.h` — client control listener.
- `src/platform/linux/http_dashboard.c/.h` — HTTP/SSE front-end + dispatcher glue.
- `src/platform/linux/dashboard/index.html` (or an embedded asset) — UI.

Changed:
- `src/platform/linux/control_socket.c/.h` — extract the shared dispatcher; keep
  the server command set.
- `src/platform/linux/platform_linux.c` — create the client listener/dashboard
  from the client run path; config plumbing for bind addr/port/auth.
- `src/vpn_client.h`, `src/config.c/.h`, `src/main.c` — `[Dashboard]` config
  keys (`Listen`, `Enabled`, `Token`, `DashboardDir`) and policy defaults.
- `src/mqvpn_client.c` — `get_status`/`get_stats`/`get_policy`/`set_policy`
  handlers; apply knobs to the scheduler.
- `src/mqvpn_internal.h` — client policy struct and accessors.
- xquic fork: runtime scheduler-params callback + `xqc_conn_set_scheduler_params`
  (see §6); WLB reads the knobs.
- `tests/test_control_socket.c`, a new client-control test, `tests/test_config.c`.
- `docs/control-api.md` — new client commands; `website/` EN + `ja/` docs.

---

## 11. Phases

1. **D1 — client control socket** (read-only: `get_status`/`get_stats`/
   `get_policy`), shared dispatcher extraction. No behaviour change.
2. **D2 — runtime knobs**: xquic runtime-params path + `set_policy`; validate,
   apply, persist.
3. **D3 — HTTP + dashboard**: static page, `/api/status` polling, cumulative
   usage, distribution graph.
4. **D4 — real-time push** (SSE) and history ring.
5. **D5 — server-side view** (optional): expose the download-direction
   distribution and allow server knob updates.
6. **D6 — auth/TLS story** and docs.

D1–D3 deliver a usable dashboard; D2 is what makes the thresholds live-tunable.

---

## 12. Open questions

- **W1 — HTTP server scope**: embed in mqvpn, or ship a tiny separate daemon?
  (Recommended: embed, loopback by default.)
- **W2 — auth**: bearer token vs basic auth vs reverse-proxy-only?
- **W3 — history**: browser-side only, or a server ring buffer for reload
  persistence?
- **W4 — bind default**: loopback always, or allow a configured address with a
  mandatory token?
- **W5 — server-side dashboard**: needed, or client-only?
- **W6 — asset delivery**: embedded vs `DashboardDir`.

---

## 13. References

| Topic | Location |
|-------|----------|
| Server control API | `src/platform/linux/control_socket.c` (command table L472), `.h` |
| Control API wiring | `src/platform/linux/platform_linux.c:1259-1264` |
| CLI status reader | `src/platform/posix/status.c:350`; `src/main.c:369` |
| Client config (no control fields) | `src/vpn_client.h` |
| No per-conn settings setter | `xquic.h:2060` (`xqc_server_set_conn_settings`); params at `xquic.h:951` |
| Scheduler callback struct | `xquic.h:1088`; WLB callback `xqc_scheduler_wlb.c:1417` |
| libevent on client + server | DD §1 |
| Control API compatibility | DD §14 |
| Runtime knob definitions | traffic-shaping-plan §10 Q5 |
| One-off deployment | traffic-shaping-plan §4.1 |
