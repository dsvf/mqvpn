#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mqvpn contributors
#
# switch-mqvpn.sh — swap a host's mqvpn binary between the unmodified
# original and this fork's build, over the WireGuard management link.
#
# Why WireGuard: the management link MUST NOT be routed through mqvpn, or a
# service restart would drop the SSH/SCP connection mid-switch. Reach the
# hosts by their WireGuard peer IPs.
#
# The remote binary is replaced atomically (install -m755) and the matching
# systemd service(s) are restarted.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BACKUP_DIR="$SCRIPT_DIR/backup"

# ============================ CONFIG (EDIT) ============================
# Local fork build output.
FORK_BIN="$SCRIPT_DIR/../build/mqvpn"

# WireGuard peer IPs (or hostnames reachable over the management link only).
SERVER_HOST="__SERVER_WG_IP__"
CLIENT_HOST="__CLIENT_WG_IP__"

SSH_USER="root"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15)

# Remote install path and systemd units (see systemd/*.service.in).
REMOTE_BIN="/usr/local/bin/mqvpn"
SERVER_SERVICE="mqvpn-server"
CLIENT_SERVICE="mqvpn-client@0"   # instance id — set to your unit's %i
# =======================================================================

# Environment overrides, e.g.:
#   SERVER_HOST=10.0.0.1 CLIENT_HOST=10.0.0.2 SSH_USER=root ./switch-mqvpn.sh status
SERVER_HOST="${SERVER_HOST:-__SERVER_WG_IP__}"
CLIENT_HOST="${CLIENT_HOST:-__CLIENT_WG_IP__}"
SSH_USER="${SSH_USER:-root}"

info() { echo "[*] $*"; }
ok()   { echo "[+] $*"; }
err()  { echo "[!] $*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: $0 <command> [target]

Commands:
  backup                    Fetch each host's current $REMOTE_BIN into
                            backup/<host>/mqvpn.orig (the "original").
  fork    [server|client]   Deploy this fork's build (default: both).
  original [server|client]  Deploy the backed-up original (default: both).
  status                    Show which build each host currently runs.

Edit the CONFIG block at the top of this script, or override via
SERVER_HOST / CLIENT_HOST / SSH_USER environment variables.
EOF
    exit "${1:-0}"
}

require_hosts() {
    if [ "$SERVER_HOST" = "__SERVER_WG_IP__" ] || [ "$CLIENT_HOST" = "__CLIENT_WG_IP__" ]; then
        err "edit SERVER_HOST/CLIENT_HOST (or export them) at the top of $0"
    fi
}

ssh_cmd() { ssh "${SSH_OPTS[@]}" "$SSH_USER@$1" "${@:2}"; }

remote_sha() { ssh_cmd "$1" "sha256sum '$REMOTE_BIN' 2>/dev/null | cut -d' ' -f1"; }

local_sha() {
    if [ -f "$1" ]; then sha256sum "$1" | cut -d' ' -f1; else echo "missing"; fi
}

host_vars() { # <server|client> -> prints "HOST SERVICE"
    case "$1" in
        server) printf '%s %s' "$SERVER_HOST" "$SERVER_SERVICE" ;;
        client) printf '%s %s' "$CLIENT_HOST" "$CLIENT_SERVICE" ;;
        *) usage 1 ;;
    esac
}

targets() { # <server|client|both> -> prints one role per line
    case "$1" in
        server) printf 'server\n' ;;
        client) printf 'client\n' ;;
        both)   printf 'server\nclient\n' ;;
        *) usage 1 ;;
    esac
}

backup_one() { # <host>
    local host="$1"
    mkdir -p "$BACKUP_DIR/$host"
    info "backing up $host:$REMOTE_BIN -> $BACKUP_DIR/$host/mqvpn.orig"
    scp "${SSH_OPTS[@]}" "$SSH_USER@$host:$REMOTE_BIN" "$BACKUP_DIR/$host/mqvpn.orig"
    ok "backed up $host"
}

deploy() { # <host> <service> <local_bin> <label>
    local host="$1" svc="$2" bin="$3" label="$4"
    [ -f "$bin" ] || err "$label binary not found: $bin (run ./build.sh in the fork first)"
    info "deploying $label to $host ($svc)"
    scp "${SSH_OPTS[@]}" "$bin" "$SSH_USER@$host:/tmp/mqvpn.switch.new"
    ssh_cmd "$host" \
        "install -m755 /tmp/mqvpn.switch.new '$REMOTE_BIN' && systemctl restart '$svc' && rm -f /tmp/mqvpn.switch.new"
    ok "$host now runs $label ($svc restarted)"
}

cmd_backup() {
    require_hosts
    backup_one "$SERVER_HOST"
    backup_one "$CLIENT_HOST"
}

cmd_switch() { # <fork|original> <target>
    local which="$1" target="${2:-both}"
    require_hosts
    local fork_sha
    fork_sha="$(local_sha "$FORK_BIN")"
    while read -r t; do
        read -r host svc < <(host_vars "$t")
        if [ "$which" = "fork" ]; then
            deploy "$host" "$svc" "$FORK_BIN" "fork"
        else
            local orig="$BACKUP_DIR/$host/mqvpn.orig"
            [ -f "$orig" ] || err "no backup for $host — run '$0 backup' first"
            deploy "$host" "$svc" "$orig" "original"
        fi
    done < <(targets "$target")
}

cmd_status() {
    require_hosts
    local fork_sha orig_sha remote state
    fork_sha="$(local_sha "$FORK_BIN")"
    for t in server client; do
        read -r host svc < <(host_vars "$t")
        orig_sha="$(local_sha "$BACKUP_DIR/$host/mqvpn.orig")"
        remote="$(remote_sha "$host" 2>/dev/null)" || remote="unreachable"
        state="unknown"
        if [ "$remote" = "$fork_sha" ]; then
            state="FORK"
        elif [ "$orig_sha" != "missing" ] && [ "$remote" = "$orig_sha" ]; then
            state="original"
        elif [ "$remote" = "unreachable" ]; then
            state="unreachable"
        fi
        printf '%-8s %-40s %s\n' "$t:" "$host" "$state"
    done
}

case "${1:-}" in
    backup)   cmd_backup ;;
    fork)     cmd_switch fork "${2:-both}" ;;
    original) cmd_switch original "${2:-both}" ;;
    status)   cmd_status ;;
    -h|--help|help|"") usage ;;
    *) echo "unknown command '$1'" >&2; usage 1 ;;
esac
