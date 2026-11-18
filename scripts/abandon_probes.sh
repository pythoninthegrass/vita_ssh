#!/usr/bin/env bash
# TASK-034: clients that disconnect while a long command runs must not keep their slots.
#
# Usage:
#   scripts/abandon_probes.sh user@host [port]     (port defaults to 22)
#
# Starts a long command on session A, then runs 5 probes that authenticate, queue a
# command and are killed (a freed slot shows in sshd.log as `dropped ... (while a
# command ran)`, stamped by the log), then checks that a sixth, real client is still accepted.
# Read sshd.log afterwards for `refused` and `dropped ... (while a command ran)` lines.
set -u

export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

TARGET=${1:-}
PORT=${2:-22}
[ -n "$TARGET" ] || { sed -n '2,10p' "$0"; exit 2; }
PROBES=5
SSH_OPTS=(-p "$PORT" -o BatchMode=yes -o ConnectTimeout=15 -o StrictHostKeyChecking=accept-new)

pass=0
fail=0
check() {
  if [ "$2" = 0 ]; then pass=$((pass + 1)); echo "ok   $1"; else fail=$((fail + 1)); echo "FAIL $1"; fi
}

# A walk of the whole card yields for minutes, which is the long command.
ssh "${SSH_OPTS[@]}" "$TARGET" "du /ux0" >/dev/null 2>&1 &
long=$!
trap 'kill "$long" 2>/dev/null' EXIT
sleep 3

for n in $(seq "$PROBES"); do
  # `sleep 1` queues behind the long command; the client is killed once it is queued.
  ssh "${SSH_OPTS[@]}" "$TARGET" "sleep 1" >/dev/null 2>&1 &
  probe=$!
  sleep 4
  kill -9 "$probe" 2>/dev/null
  wait "$probe" 2>/dev/null
  echo "probe $n killed"
done

# The sixth client's command waits behind the long one, so success is an
# accepted, authenticated connection that is still waiting, not an exit status.
ssh "${SSH_OPTS[@]}" "$TARGET" "echo alive" >/dev/null 2>&1 &
sixth=$!
sleep 10
kill -0 "$sixth" 2>/dev/null
check "sixth client is accepted and waiting behind the long command" $?
kill "$sixth" 2>/dev/null
kill -0 "$long" 2>/dev/null
check "long command on session A is still running" $?

echo "$pass pass $fail fail"
[ "$fail" = 0 ]
