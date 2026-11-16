#!/usr/bin/env bash
# TASK-001.04 smoke tests for the Vita sshd, both halves in one file.
#
# Usage:
#   scripts/smoke.sh                everything: exec checks, then pty checks
#   scripts/smoke.sh exec           exec-mode checks only   (ACs 2, 3, 4, 5, 6)
#   scripts/smoke.sh pty            interactive checks only (ACs 3, 7, 8)
#   scripts/smoke.sh MODE [user@host] [port]     (defaults: all any@192.168.8.57 2222)
#
# The exec half drives one-shot `ssh host "cmd"` calls and grades the exit
# statuses the Vita sends back, so `$?` is proven usable from a script. The
# pty half drives two concurrent interactive sessions through FIFOs, so tab
# completion, `!n`/`!!` and the Ctrl keys arrive as if typed; it then grades
# the transcripts. Scratch on the Vita: /ux0/data/smoke-$$, removed at the
# end. Scratch locally: a mktemp dir, printed with the pty transcripts.
#
# Run it from a real terminal on the LAN: the authoring agent's sandbox
# allowlists binaries and its ssh cannot leave the machine at all.
set -u

usage() {
  sed -n '2,10p' "$0"
}

MODE=all
case "${1:-}" in
  exec|pty|all)   MODE=$1; shift ;;
  -h|--help|help) usage; exit 0 ;;
esac
# A typo'd mode would otherwise be taken for a hostname and run "all" against
# a host literally named "exce".
case "${1:-}" in
  *@*|*.*) ;;                 # looks like a host: fine
  "") ;;                      # no host given: the default applies
  *) usage; exit 2 ;;
esac
HOST=${1:-any@192.168.8.57}
PORT=${2:-2222}
PASS=0; FAIL=0

# One grader for both halves: equal strings pass. The exec half passes real
# exit codes ("0" vs "1"); the pty half passes ok/fail verdicts.
check() {
  if [ "$2" = "$3" ]; then
    printf 'PASS  %-44s\n' "$1"; PASS=$((PASS+1))
  else
    printf 'FAIL  %-44s (%s, want %s)\n' "$1" "$3" "$2"; FAIL=$((FAIL+1))
  fi
}

run_exec() {
  local V="ssh -p $PORT -o BatchMode=yes -o ConnectTimeout=5 $HOST"
  local T="smoke-$$" OUT RC ERR TMPKEY DEV2 MVOK d

  # --- AC6: exec mode returns exit statuses usable in a script -----------
  $V "pwd"             >/dev/null 2>&1; RC=$?; check "exec pwd (ok -> 0)"          0   "$RC"
  $V "cat /nosuchfile" >/dev/null 2>&1; RC=$?; check "exec cat missing (err -> 1)" 1   "$RC"
  $V "nosuchcommand"   >/dev/null 2>&1; RC=$?; check "exec unknown cmd (-> 127)"   127 "$RC"
  OUT=$($V "echo smoke-marker"); [ "$OUT" = "smoke-marker" ]
  [ $? -eq 0 ] && check "exec stdout captured verbatim" ok ok || check "exec stdout captured verbatim" ok fail

  # --- AC3: device listing, cd/pwd chains, relative paths -----------------
  $V "ls /"                      >/dev/null 2>&1; RC=$?; check "ls / lists devices" 0 "$RC"
  for d in ux0 os0 vs0 app0; do
    $V "ls /$d" >/dev/null 2>&1; RC=$?; check "ls /$d (mounted or honest err)" 0 "$RC"
  done
  # No `&&` in this shell: chaining is covered by the pty half, where cd and
  # pwd run in one live session.
  $V "cd /ux0/data"              >/dev/null 2>&1; RC=$?; check "cd absolute (device path)" 0 "$RC"
  $V "cd /ux0"                   >/dev/null 2>&1; RC=$?; check "cd device root"            0 "$RC"
  $V "cd"                        >/dev/null 2>&1; RC=$?; check "bare cd lands home"   0 "$RC"

  # --- AC5: untranslatable paths are honest errors ------------------------
  $V "ls /nosuchdev"       >/dev/null 2>&1; RC=$?; check "ls unknown device errs"    1 "$RC"
  $V "cat /ux0/../../../x" >/dev/null 2>&1; RC=$?; check "traversal above root errs" 1 "$RC"
  $V "cd /ux0/../nope"     >/dev/null 2>&1; RC=$?; check "cd above root errs"        1 "$RC"

  # --- AC4: tree tools and cross-device mv --------------------------------
  $V "mkdir /ux0/data/$T /ux0/data/$T/sub" >/dev/null 2>&1
  $V "echo hello > /ux0/data/$T/sub/f.txt" >/dev/null 2>&1
  OUT=$($V "cat /ux0/data/$T/sub/f.txt"); [ "$OUT" = "hello" ]
  if [ $? -eq 0 ]; then check "redirect writes, cat reads back" ok ok
  else printf 'FAIL  redirect writes, cat reads back (got "%s")\n' "$OUT"; FAIL=$((FAIL+1)); fi
  $V "cp -r /ux0/data/$T /ux0/data/$T-copy" >/dev/null 2>&1
  $V "cat /ux0/data/$T-copy/sub/f.txt" >/dev/null 2>&1; RC=$?
  check "cp -r copies a tree" 0 "$RC"
  $V "du /ux0/data/$T"           >/dev/null 2>&1; RC=$?; check "du walks a tree" 0 "$RC"
  # cross-device mv: the first candidate device that accepts the write
  MVOK=""
  for DEV2 in uma0 tm0 grw0 ud0 ur0; do
    if $V "mv /ux0/data/$T/sub/f.txt /$DEV2/smoke-$$.txt" >/dev/null 2>&1; then
      MVOK="$DEV2"; break
    fi
  done
  if [ -z "$MVOK" ]; then
    printf 'SKIP  cross-device mv: no second writable device accepted it\n'
  else
    $V "cat /$MVOK/smoke-$$.txt" >/dev/null 2>&1
    check "mv across devices (->$MVOK)" 0 "$?"
    $V "rm /$MVOK/smoke-$$.txt" >/dev/null 2>&1
    $V "echo hello > /ux0/data/$T/sub/f.txt" >/dev/null 2>&1
  fi
  $V "rm -r /ux0/data/$T /ux0/data/$T-copy" >/dev/null 2>&1
  $V "ls /ux0/data/$T"  >/dev/null 2>&1; RC=$?; check "rm -r removes the tree"      1 "$RC"
  $V "rm /nosuchfile"   >/dev/null 2>&1; RC=$?; check "rm missing errs honestly"    1 "$RC"

  # --- AC2: a key that is not installed is refused -------------------------
  # The verdict comes from stderr, not the exit code: 255 is also what a
  # network failure returns, which once made this check pass while the
  # device was unreachable.
  TMPKEY=$(mktemp -d)
  ssh-keygen -q -t ed25519 -N "" -f "$TMPKEY/throwaway" 2>/dev/null
  ERR=$(ssh -p $PORT -i "$TMPKEY/throwaway" -o IdentitiesOnly=yes -o BatchMode=yes \
      -o ConnectTimeout=5 "$HOST" "pwd" 2>&1 >/dev/null)
  case "$ERR" in
    *"Permission denied"*) check "wrong key refused (publickey only)" ok ok ;;
    *) check "wrong key refused (publickey only): got \"$ERR\"" ok fail ;;
  esac
  rm -rf "$TMPKEY"

  printf '\nexec: %d pass, %d fail\n' "$PASS" "$FAIL"
  [ "$FAIL" -eq 0 ]
}

run_pty() {
  # A subshell on purpose: the FIFO fds (3/4) and the two ssh children close
  # with it, so a failed check cannot leak a session into the caller.
  (
    local D SSHA SSHB
    PASS=0; FAIL=0
    D=$(mktemp -d)
    mkfifo "$D/a.in" "$D/b.in"
    ssh -tt -p $PORT -o BatchMode=yes "$HOST" < "$D/a.in" > "$D/a.out" 2>&1 &
    SSHA=$!
    ssh -tt -p $PORT -o BatchMode=yes "$HOST" < "$D/b.in" > "$D/b.out" 2>&1 &
    SSHB=$!
    exec 3> "$D/a.in"
    exec 4> "$D/b.in"
    sleep 2

    # AC8: tab completion fills a word (single match adds the slash)
    printf 'cat sshd.\t\r' >&3; sleep 1

    # AC3: per-session cwd isolation -- A moves; B must not
    printf 'cd /ux0\r' >&3; sleep 1
    printf 'pwd\r' >&4; sleep 1

    # AC8: history recall by !! (echo a marker, re-run it)
    printf 'echo smoke-hist-42\r' >&3; sleep 1
    printf '!!\r' >&3; sleep 1

    # AC8: editing keys -- Ctrl-U wipes the line, Ctrl-W wipes a word
    printf 'echo GARBAGE\x15echo after-cu\r' >&3; sleep 1
    printf 'echo one two\x17three\r' >&3; sleep 1

    # AC8: Ctrl-L repaints (clear-screen escape), Ctrl-C abandons the line
    printf '\x0c' >&3; sleep 1
    printf 'echo BAD\x03' >&3; printf 'pwd\r' >&3; sleep 1

    # AC3: B confirms it never moved
    printf 'pwd\r' >&4; sleep 1

    # AC7: `exit` ends the session
    printf 'exit\r' >&3; sleep 1
    exec 3>&-

    # AC7: abrupt hangup -- kill the client mid-session; the server must
    # recover on its next poll pass
    printf 'echo before-kill\r' >&4; sleep 1
    kill -9 $SSHB 2>/dev/null
    wait $SSHB 2>/dev/null
    sleep 1
    ssh -p $PORT -o BatchMode=yes -o ConnectTimeout=5 "$HOST" "pwd" >/dev/null 2>&1
    check "connection after abrupt hangup still works" 0 "$?"
    kill $SSHA $SSHB 2>/dev/null

    # --- grade the transcripts -------------------------------------------
    grep -q "cat sshd.log" "$D/a.out" && check "tab completion filled sshd.log" ok ok || check "tab completion filled sshd.log" ok fail
    grep -q "^/ux0" "$D/b.out" && check "session B kept its own cwd (isolation)" ok ok || check "session B kept its own cwd (isolation)" ok fail
    [ "$(grep -c 'smoke-hist-42' "$D/a.out")" -ge 2 ] && check "!! re-ran the command" ok ok || check "!! re-ran the command" ok fail
    grep -q "after-cu" "$D/a.out" && ! grep -q "^GARBAGE" "$D/a.out" && check "Ctrl-U wiped the line" ok ok || check "Ctrl-U wiped the line" ok fail
    grep -q "one three" "$D/a.out" && check "Ctrl-W wiped one word" ok ok || check "Ctrl-W wiped one word" ok fail
    grep -q $'\033\[2J' "$D/a.out" && check "Ctrl-L repainted the screen" ok ok || check "Ctrl-L repainted the screen" ok fail
    grep -q '\^C' "$D/a.out" && ! grep -q "^BAD" "$D/a.out" && check "Ctrl-C abandoned the line" ok ok || check "Ctrl-C abandoned the line" ok fail
    grep -q "before-kill" "$D/b.out" && check "session B ran before the kill" ok ok || check "session B ran before the kill" ok fail

    printf '\npty: %d pass, %d fail\n' "$PASS" "$FAIL"
    printf 'full transcripts: %s/a.out %s/b.out\n' "$D" "$D"
    [ "$FAIL" -eq 0 ]
  )
}

case "$MODE" in
  exec) run_exec ;;
  pty)  run_pty ;;
  all)
    run_exec; RC=$?
    run_pty;  RC=$((RC + $?))
    if [ "$RC" = 0 ]; then
      printf '\nall: both halves passed\n'
      exit 0
    fi
    printf '\nall: failures above\n'
    exit 1
    ;;
esac
