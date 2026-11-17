#!/usr/bin/env bash
# TASK-012: silent sockets must not stall or starve the Vita sshd.
#
# Usage:
#   scripts/silent_sockets.sh user@host [port]     (port defaults to 22)
#
# Opens 8 TCP connections that send nothing, then checks that a real client
# still completes key exchange and that 20 consecutive one-shot connections
# all return 0. The silent sockets are held for the whole run and closed at
# the end. Run it from a real terminal on the LAN.
set -u

export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

TARGET=${1:-}
PORT=${2:-22}
[ -n "$TARGET" ] || { sed -n '2,9p' "$0"; exit 2; }
HOST=${TARGET#*@}
SILENT=8
ONESHOTS=20
SSH_OPTS=(-p "$PORT" -o BatchMode=yes -o ConnectTimeout=15 -o StrictHostKeyChecking=accept-new)

pass=0
fail=0
check() {
  if [ "$2" = 0 ]; then pass=$((pass + 1)); echo "ok   $1"; else fail=$((fail + 1)); echo "FAIL $1"; fi
}

holders=()
cleanup() { for p in "${holders[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

for n in $(seq "$SILENT"); do
  sleep 300 | nc "$HOST" "$PORT" >/dev/null 2>&1 &
  holders+=("$!")
done
sleep 2

out=$(timeout 40 ssh "${SSH_OPTS[@]}" "$TARGET" pwd 2>&1)
check "ssh pwd with $SILENT silent sockets open (got: $out)" $?

for n in $(seq "$ONESHOTS"); do
  timeout 40 ssh "${SSH_OPTS[@]}" "$TARGET" pwd >/dev/null 2>&1
  check "one-shot $n of $ONESHOTS" $?
done

echo "$pass pass $fail fail"
[ "$fail" = 0 ]
