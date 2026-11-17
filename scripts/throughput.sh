#!/usr/bin/env bash
# TASK-002.03.03 throughput baseline for the Vita sshd. See docs/throughput.md.
#
# Usage:
#   scripts/throughput.sh [MODE] user@host [port]     (MODE defaults to bench, port to 22)
#
#   MODE bench  RUNS rounds of scp up, scp down, sftp put, sftp get on a 12 MB file;
#               one THROUGHPUT line per transfer, then median/min/max per direction
#   MODE limit  what bounds the rate: on-device cp (card, no network) against exec cat
#               under each cipher (crypto), RUNS rounds with the ciphers interleaved
#
# RUNS defaults to 5. LINK_NOTE records the Vita's distance and channel, which no
# tool can read. Every transfer runs next to a 0.2 s ping, so a slow transfer can be
# matched against loss and latency on the link. Run it from a real terminal on the
# LAN, with the client sitting where it will be for every later run.
set -u

# Apple's OpenSSH ahead of Homebrew's: brew's ssh cannot reach the Vita, and brew
# is first in PATH on the mini, so a baseline run that trusts PATH would measure
# the wrong client. See scripts/smoke.sh for the same guard.
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

usage() {
  sed -n '2,15p' "$0"
}

MODE=bench
case "${1:-}" in
  bench|limit) MODE=$1; shift ;;
  -h|--help|help) usage; exit 0 ;;
esac
# A typo'd mode would otherwise be taken for a hostname.
case "${1:-}" in
  *@*|*.*) ;;
  *) usage; exit 2 ;;
esac
HOST=$1
PORT=${2:-22}
RUNS=${RUNS:-5}
SIZE=12582912
SSHO="-o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
REMOTE=/ux0/data/throughput-$$
LOCAL=$(mktemp -d)
trap 'rm -rf "$LOCAL"' EXIT
FAILED=0

. "$(dirname "$0")/transfer_lib.sh"

sftp_run() { printf '%s\n' "$1" | sftp -q $SSHO -P "$PORT" -b - "$HOST" 2>"$LOCAL/stderr"; }

# Starts a background ping next to a transfer; probe_stop prints its loss and latency.
probe_start() { ping -i 0.2 "${HOST#*@}" >"$LOCAL/ping.out" 2>&1 & PINGPID=$!; }
probe_stop() {
  kill -INT "$PINGPID" 2>/dev/null; wait "$PINGPID" 2>/dev/null
  printf '      link: %s, rtt avg/max %s ms\n' \
    "$(grep -o '[0-9.]*% packet loss' "$LOCAL/ping.out" | sed 's/ packet//')" \
    "$(grep 'min/avg/max' "$LOCAL/ping.out" | sed 's/.* = //' | awk -F'[/ ]' '{ print $2 "/" $3 }')"
}

# One timed transfer: label, bytes, then the command. Failures are counted, not fatal.
transfer() {
  local label=$1 bytes=$2; shift 2
  : > "$LOCAL/stderr"
  probe_start
  timed "$@" >/dev/null 2>>"$LOCAL/stderr"
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    printf 'FAIL  %s (exit %s)\n' "$label" "$rc"; head -3 "$LOCAL/stderr" | sed 's/^/      stderr: /'
    FAILED=$((FAILED+1))
  else
    rate "$label" "$(basename "$LOCAL/data.bin")" "$bytes" | tee -a "$LOCAL/rates"
  fi
  probe_stop
}

# median, min and max of the rates logged under one label.
summarise() {
  grep " $1 " "$LOCAL/rates" 2>/dev/null | awk '{ print $9 }' | sort -n | awk -v l="$1" -v d="$(date +%Y-%m-%d)" '
    { v[NR] = $1 }
    END {
      if (NR == 0) { printf "SUMMARY %s %s n=0\n", d, l; exit }
      m = (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2
      printf "SUMMARY %s %s n=%d median %.2f min %.2f max %.2f MB/s\n", d, l, NR, m, v[1], v[NR]
    }'
}

conditions() {
  local vpk="$(dirname "$0")/../build/ssh_server_vita.vpk"
  printf 'CONDITIONS\n'
  printf '  date:    %s\n' "$(date '+%Y-%m-%d %H:%M %Z')"
  printf '  client:  %s on %s\n' "$(ssh -V 2>&1 | cut -d, -f1)" "$(uname -sr)"
  printf '  server:  %s, local build %s\n' "$(git -C "$(dirname "$0")" describe --always --dirty 2>/dev/null || echo unknown)" \
    "$([ -f "$vpk" ] && sha "$vpk" | cut -c1-12 || echo "no VPK")"
  printf '  target:  %s port %s, file %s bytes, %s rounds\n' "$HOST" "$PORT" "$SIZE" "$RUNS"
  # The Vita's radio cannot be queried, so its distance and channel come from LINK_NOTE.
  printf '  client link: %s\n' "$(route -n get "${HOST#*@}" 2>/dev/null | awk '/interface:/ { print $2 }')"
  printf '  vita link:   %s\n' "${LINK_NOTE:-not recorded; set LINK_NOTE=\"vita 2 m from AP, ch 36\"}"
  ping -c 20 -i 0.2 "${HOST#*@}" 2>&1 | grep -E 'packet loss|min/avg/max' | sed 's/^/  idle ping: /'
}

setup() {
  head -c "$SIZE" /dev/urandom > "$LOCAL/data.bin"
  : > "$LOCAL/rates"
  if ! sftp_run "mkdir $REMOTE" >/dev/null; then
    printf 'cannot open an sftp session on %s:%s\n' "$HOST" "$PORT"; head -3 "$LOCAL/stderr"; exit 1
  fi
}

teardown() {
  ssh -p "$PORT" $SSHO "$HOST" "rm -r $REMOTE" >/dev/null 2>&1
}

run_bench() {
  conditions
  setup
  local i
  for i in $(seq 1 "$RUNS"); do
    printf 'round %s of %s\n' "$i" "$RUNS"
    transfer scp-up "$SIZE" scp -s -q $SSHO -P "$PORT" "$LOCAL/data.bin" "$HOST:$REMOTE/scp.bin"
    transfer scp-down "$SIZE" scp -s -q $SSHO -P "$PORT" "$HOST:$REMOTE/scp.bin" "$LOCAL/scp.back"
    transfer sftp-put "$SIZE" sftp_run "put $LOCAL/data.bin $REMOTE/sftp.bin"
    transfer sftp-get "$SIZE" sftp_run "get $REMOTE/sftp.bin $LOCAL/sftp.back"
  done
  # The last round's copies prove the timed transfers moved the right bytes.
  [ "$(sha "$LOCAL/data.bin")" = "$(sha "$LOCAL/scp.back" 2>/dev/null)" ] || { printf 'FAIL  scp round trip differs\n'; FAILED=$((FAILED+1)); }
  [ "$(sha "$LOCAL/data.bin")" = "$(sha "$LOCAL/sftp.back" 2>/dev/null)" ] || { printf 'FAIL  sftp round trip differs\n'; FAILED=$((FAILED+1)); }
  printf '\n'
  for d in scp-up scp-down sftp-put sftp-get; do summarise "$d"; done
  teardown
}

# Rates net of the session setup: one `pwd` costs a handshake, and a 12 MB cp on the card
# can finish in less time than the handshake takes.
run_limit() {
  conditions
  setup
  local i c ovh=0 ciphers="aes128-ctr aes256-ctr aes128-gcm@openssh.com aes256-gcm@openssh.com chacha20-poly1305@openssh.com"
  scp -s -q $SSHO -P "$PORT" "$LOCAL/data.bin" "$HOST:$REMOTE/t.bin" 2>"$LOCAL/stderr" ||
    { printf 'upload failed\n'; head -3 "$LOCAL/stderr"; exit 1; }
  for i in $(seq 1 "$RUNS"); do
    printf 'round %s of %s\n' "$i" "$RUNS"
    timed ssh -p "$PORT" $SSHO "$HOST" "pwd" >/dev/null 2>&1; ovh=$ELAPSED
    # cp reads and writes the card inside the Vita: no network, no crypto in the data path.
    probe_start
    timed ssh -p "$PORT" $SSHO "$HOST" "cp $REMOTE/t.bin $REMOTE/c.bin" >/dev/null 2>"$LOCAL/stderr"
    probe_stop
    ELAPSED=$(awk -v e="$ELAPSED" -v o="$ovh" 'BEGIN { printf "%.2f", (e > o + 0.05) ? e - o : 0.05 }')
    rate card-cp "$(basename "$LOCAL/data.bin")" "$SIZE" | tee -a "$LOCAL/rates"
    for c in $ciphers; do
      probe_start
      timed ssh -p "$PORT" $SSHO -c "$c" "$HOST" "cat $REMOTE/t.bin" > "$LOCAL/cat.out" 2>"$LOCAL/stderr"
      local rc=$?
      probe_stop
      if [ "$rc" -ne 0 ]; then
        printf 'SKIP  %s (exit %s: %s)\n' "$c" "$rc" "$(head -1 "$LOCAL/stderr")"
      else
        rate "cat-${c%%@*}" "$(basename "$LOCAL/data.bin")" "$SIZE" | tee -a "$LOCAL/rates"
      fi
    done
  done
  printf '\n'
  summarise card-cp
  for c in $ciphers; do summarise "cat-${c%%@*}"; done
  teardown
}

case "$MODE" in
  bench) run_bench ;;
  limit) run_limit ;;
esac
[ "$FAILED" -eq 0 ]
