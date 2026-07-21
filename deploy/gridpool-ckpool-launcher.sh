#!/usr/bin/env bash
set -euo pipefail

CKPOOL_BIN="${CKPOOL_BIN:-$HOME/.local/libexec/gridpool-ckpool}"
BITCOIN_COOKIE_FILE="${BITCOIN_COOKIE_FILE:-$HOME/.bitcoin/.cookie}"
BITCOIN_RPC_HOST="${BITCOIN_RPC_HOST:-127.0.0.1:8334}"
GRIDPOOL_ADAPTER_SOCKET="${GRIDPOOL_ADAPTER_SOCKET:-$HOME/.local/run/gridpool/ckpool-adapter.sock}"
GRIDPOOL_OPERATOR_ADDRESS="${GRIDPOOL_OPERATOR_ADDRESS:?set GRIDPOOL_OPERATOR_ADDRESS}"
GRIDPOOL_FIXED_ADDRESS="${GRIDPOOL_FIXED_ADDRESS:-}"
GRIDPOOL_STRATUM_PORT="${GRIDPOOL_STRATUM_PORT:-3335}"
GRIDPOOL_DEFAULT_ENABLED="${GRIDPOOL_DEFAULT_ENABLED:-false}"
GRIDPOOL_RUNTIME_DIR="${GRIDPOOL_RUNTIME_DIR:-$HOME/.local/state/gridpool-ckpool}"

[[ -x "$CKPOOL_BIN" ]] || { echo "CKPool binary not executable: $CKPOOL_BIN" >&2; exit 1; }
[[ -s "$BITCOIN_COOKIE_FILE" ]] || { echo "Bitcoin RPC cookie missing: $BITCOIN_COOKIE_FILE" >&2; exit 1; }
[[ -S "$GRIDPOOL_ADAPTER_SOCKET" ]] || { echo "GridPool adapter socket missing: $GRIDPOOL_ADAPTER_SOCKET" >&2; exit 1; }

cookie="$(<"$BITCOIN_COOKIE_FILE")"
rpc_user="${cookie%%:*}"
rpc_pass="${cookie#*:}"
mkdir -p "$GRIDPOOL_RUNTIME_DIR"
chmod 700 "$GRIDPOOL_RUNTIME_DIR"
config="$GRIDPOOL_RUNTIME_DIR/ckpool.runtime.json"

jq -n \
  --arg rpc_host "$BITCOIN_RPC_HOST" \
  --arg rpc_user "$rpc_user" \
  --arg rpc_pass "$rpc_pass" \
  --arg fallback "$GRIDPOOL_OPERATOR_ADDRESS" \
  --arg server "0.0.0.0:$GRIDPOOL_STRATUM_PORT" \
  --arg socket "$GRIDPOOL_ADAPTER_SOCKET" \
  --arg operator "$GRIDPOOL_OPERATOR_ADDRESS" \
  --arg fixed "$GRIDPOOL_FIXED_ADDRESS" \
  --argjson default_enabled "$GRIDPOOL_DEFAULT_ENABLED" \
  '{
    btcd: [{url: $rpc_host, auth: $rpc_user, pass: $rpc_pass, notify: true}],
    btcaddress: $fallback,
    serverurl: [$server],
    update_interval: 10,
    gridpool_enabled: true,
    gridpool_default_enabled: $default_enabled,
    gridpool_adapter_socket: $socket,
    gridpool_operator_address: $operator,
    gridpool_max_coinbase_bytes: 65536,
    gridpool_adapter_max_message_bytes: 262144
  } + (if $fixed == "" then {} else {gridpool_fixed_address: $fixed} end)' >"$config"
chmod 600 "$config"

exec "$CKPOOL_BIN" -B -c "$config"
