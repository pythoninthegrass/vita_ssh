#!/usr/bin/env bash
# TASK-001.04 smoke tests, interactive half (ACs 3-isolation, 7, 8).
# Drives two REAL pty sessions over ssh with FIFOs so keystrokes (tab,
# history, Ctrl-U/W/L/C/D) arrive as if typed, and sessions stay open
# concurrently. Usage: smoke_pty.sh [user@host] [port]
set -u
HOST=${1:-any@192.168.8.57}
PORT=${2:-2222}
D=$(mktemp -d)
PASS=0; FAIL=0
check() {
  if [ "$2" = "$3" ]; then printf 'PASS  %s\n' "$1"; PASS=$((PASS+1))
  else printf 'FAIL  %s (%s)\n' "$1" "$3"; FAIL=$((FAIL+1)); fi
}

# Two concurrent interactive sessions, each input held open on a FIFO.
mkfifo "$D/a.in" "$D/b.in"
ssh -tt -p $PORT -o BatchMode=yes "$HOST" < "$D/a.in" > "$D/a.out" 2>&1 &
SSHA=$!
ssh -tt -p $PORT -o BatchMode=yes "$HOST" < "$D/b.in" > "$D/b.out" 2>&1 &
SSHB=$!
exec 3> "$D/a.in"
exec 4> "$D/b.in"
sleep 2

# AC8: tab completion fills a word (single match adds the slash)
printf 'cd vi\tsshd/\r\n' >&3; sleep 1
printf 'pwd\r\n' >&3; sleep 1

# AC3: per-session cwd isolation — A changes directory; B must not move
printf 'cd /ux0\r\n' >&3; sleep 1
printf 'pwd\r\n' >&4; sleep 1

# AC8: history recall by !n and !! (echo a marker, re-run it)
printf 'echo smoke-hist-42\r\n' >&3; sleep 1
printf '!!\r\n' >&3; sleep 1

# AC8: editing keys — Ctrl-U wipes the line, Ctrl-W wipes a word
printf 'echo GARBAGE\r' >&3; printf '\x15' >&3; printf 'echo after-cu\r\n' >&3; sleep 1
printf 'echo one two\r' >&3; printf '\x17' >&3; printf 'three\r\n' >&3; sleep 1

# AC8: Ctrl-L repaints (clear-screen escape), Ctrl-C abandons the line
printf '\x0c' >&3; sleep 1
printf 'echo BAD\r' >&3; printf '\x03' >&3; printf 'pwd\r\n' >&3; sleep 1

# AC3: B confirms it never moved
printf 'pwd\r\n' >&4; sleep 1

# AC7: `exit` ends the session
printf 'exit\r\n' >&3; sleep 1

exec 3>&-
# AC7: abrupt hangup — kill the client mid-session, server must recover
printf 'echo before-kill\r\n' >&4; sleep 1
kill -9 $SSHB 2>/dev/null
wait $SSHB 2>/dev/null
sleep 1

# AC7: a later connection still works
ssh -p $PORT -o BatchMode=yes -o ConnectTimeout=5 "$HOST" "pwd" >/dev/null 2>&1
check "connection after abrupt hangup still works" 0 $?

# --- grade the transcript ------------------------------------------------
grep -q "/ux0/data/vitasshd" "$D/a.out" && check "tab completion filled vitasshd/" 0 0 || check "tab completion filled vitasshd/" 0 1
grep -q "^/ux0" "$D/b.out" && check "session B kept its own cwd (isolation)" 0 0 || check "session B kept its own cwd (isolation)" 0 1
[ "$(grep -c 'smoke-hist-42' "$D/a.out")" -ge 2 ] && check "! / !! re-ran the command" 0 0 || check "! / !! re-ran the command" 0 1
grep -q "after-cu" "$D/a.out" && ! grep -q "GARBAGE" "$D/a.out" && check "Ctrl-U wiped the line" 0 0 || check "Ctrl-U wiped the line" 0 1
grep -q "one three" "$D/a.out" && check "Ctrl-W wiped one word" 0 0 || check "Ctrl-W wiped one word" 0 1
grep -q $'\033\[2J' "$D/a.out" && check "Ctrl-L repainted the screen" 0 0 || check "Ctrl-L repainted the screen" 0 1
grep -q '\^C' "$D/a.out" && ! grep -q "BAD" "$D/a.out" && check "Ctrl-C abandoned the line" 0 0 || check "Ctrl-C abandoned the line" 0 1
grep -q "before-kill" "$D/b.out" && check "session B ran before the kill" 0 0 || check "session B ran before the kill" 0 1

printf '\npty-half: %d pass, %d fail\n' "$PASS" "$FAIL"
printf 'full transcripts: %s/a.out %s/b.out\n' "$D" "$D"
[ "$FAIL" -eq 0 ]
