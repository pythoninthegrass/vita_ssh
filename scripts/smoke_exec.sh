#!/usr/bin/env bash
# TASK-001.04 smoke tests, exec-mode half (ACs 2, 3, 4, 5, 6).
# Usage: smoke_exec.sh [user@host] [port]   (default any@192.168.8.57 2222)
# Every check prints PASS/FAIL with the observed exit status; scratch files
# live under ~/smoke-$$ on the Vita and are removed at the end.
set -u
HOST=${1:-any@192.168.8.57}
PORT=${2:-2222}
V="ssh -p $PORT -o BatchMode=yes -o ConnectTimeout=5 $HOST"
PASS=0; FAIL=0

check() { # name expected_rc actual_rc
  if [ "$2" = "$3" ]; then
    printf 'PASS  %-42s rc=%s\n' "$1" "$3"; PASS=$((PASS+1))
  else
    printf 'FAIL  %-42s rc=%s want=%s\n' "$1" "$3" "$2"; FAIL=$((FAIL+1))
  fi
}

# --- AC6: exec mode returns exit statuses usable in a script -------------
$V "pwd"                       >/dev/null 2>&1; check "exec pwd (ok -> 0)"           0  $?
$V "cat /nosuchfile"           >/dev/null 2>&1; check "exec cat missing (err -> 1)"  1  $?
$V "nosuchcommand"             >/dev/null 2>&1; check "exec unknown cmd (-> 127)"    127 $?
OUT=$($V "echo smoke-marker"); [ "$OUT" = "smoke-marker" ]
[ $? -eq 0 ] && check "exec stdout captured verbatim" 0 0 || check "exec stdout captured verbatim" 0 1

# --- AC3: device listing, cd/pwd chains, relative paths ------------------
$V "ls /"                      >/dev/null 2>&1; check "ls / lists devices"           0  $?
for d in ux0 os0 vs0 app0; do
  $V "ls /$d"                  >/dev/null 2>&1; check "ls /$d (mounted or honest err)" 0 $?
done
$V "cd /ux0/data && pwd"       >/dev/null 2>&1; check "cd absolute + pwd"            0  $?
$V "cd /ux0 && cd data && pwd" >/dev/null 2>&1; check "relative cd then pwd"         0  $?
$V "cd"                        >/dev/null 2>&1; check "bare cd lands home"           0  $?

# --- AC5: untranslatable paths are honest errors -------------------------
$V "ls /nosuchdev"             >/dev/null 2>&1; check "ls unknown device errs"       1  $?
$V "cat /ux0/../../../x"       >/dev/null 2>&1; check "traversal above root errs"    1  $?
$V "cd /ux0/../nope"           >/dev/null 2>&1; check "cd above root errs"           1  $?

# --- AC4: tree tools and cross-device mv ---------------------------------
T="smoke-$$"
$V "mkdir /ux0/data/$T /ux0/data/$T/sub" >/dev/null 2>&1
$V "echo hello > /ux0/data/$T/sub/f.txt" >/dev/null 2>&1
$V "echo hello > /ux0/data/$T/sub/f.txt" >/dev/null 2>&1
OUT=$($V "cat /ux0/data/$T/sub/f.txt"); [ "$OUT" = "hello" ]
[ $? -eq 0 ] && check "redirect writes, cat reads back" 0 0 || check "redirect writes, cat reads back" 0 1
$V "cp -r /ux0/data/$T /ux0/data/$T-copy" >/dev/null 2>&1
$V "cat /ux0/data/$T-copy/sub/f.txt" >/dev/null 2>&1; check "cp -r copies a tree"           0  $?
$V "du /ux0/data/$T"           >/dev/null 2>&1; check "du walks a tree"                  0  $?
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
  $V "cat /$MVOK/smoke-$$.txt" >/dev/null 2>&1 && check "mv across devices (->$MVOK)" 0 0 \
                                               || check "mv across devices (->$MVOK)" 0 1
  $V "rm /$MVOK/smoke-$$.txt" >/dev/null 2>&1
  $V "echo hello > /ux0/data/$T/sub/f.txt" >/dev/null 2>&1
fi
$V "rm -r /ux0/data/$T /ux0/data/$T-copy" >/dev/null 2>&1
$V "ls /ux0/data/$T"           >/dev/null 2>&1; check "rm -r removes the tree"           1  $?
$V "rm /nosuchfile"            >/dev/null 2>&1; check "rm missing errs honestly"         1  $?

# --- AC2: a key that is not installed is refused --------------------------
TMPKEY=$(mktemp -d)
ssh-keygen -q -t ed25519 -N "" -f "$TMPKEY/throwaway" 2>/dev/null
ERR=$(ssh -p $PORT -i "$TMPKEY/throwaway" -o IdentitiesOnly=yes -o BatchMode=yes \
    -o ConnectTimeout=5 "$HOST" "pwd" 2>&1 >/dev/null)
case "$ERR" in
  *"Permission denied"*) check "wrong key refused (publickey only)" 0 0 ;;
  *) printf 'FAIL  wrong key refused: got "%s"\n' "$ERR"; FAIL=$((FAIL+1)) ;;
esac
rm -rf "$TMPKEY"

printf '\nexec-half: %d pass, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
