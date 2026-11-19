#!/usr/bin/env bash
# TASK-001.04 and TASK-002 smoke tests for the Vita sshd, all three halves in one file.
#
# Usage:
#   scripts/smoke.sh [MODE] user@host [port]     (MODE defaults to all, port to 22)
#
#   MODE all   everything: exec, pty, then sftp checks
#   MODE exec  exec-mode checks only   (ACs 2, 3, 4, 5, 6)
#   MODE pty   interactive checks only (ACs 3, 7, 8)
#   MODE sftp  sftp and scp checks only (TASK-002.01 to TASK-002.03)
#
# The exec half drives one-shot `ssh host "cmd"` calls and grades the exit
# statuses the Vita sends back, so `$?` is proven usable from a script. The
# pty half drives two concurrent interactive sessions through FIFOs, so tab
# completion, `!n`/`!!` and the Ctrl keys arrive as if typed; it then grades
# the transcripts. The sftp half drives the stock sftp and scp clients in batch
# mode and grades exit statuses and file contents. Scratch on the Vita:
# /ux0/data/smoke-$$ and /ux0/data/smoke-sftp-$$, removed at the end. Scratch
# locally: a mktemp dir, printed with the pty transcripts.
#
# Run it from a real terminal on the LAN: the authoring agent's sandbox
# allowlists binaries and its ssh cannot leave the machine at all.
set -u

# Apple's OpenSSH ahead of Homebrew's. brew's ssh reports "No route to host" for
# the Vita where /usr/bin/ssh connects, and brew is first in PATH on the mini, so
# a run that trusts PATH grades the wrong client. brew stays on PATH behind the
# system dirs, for whatever else the script shells out to.
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

usage() {
  sed -n '2,10p' "$0"
}

MODE=all
case "${1:-}" in
  exec|pty|sftp|all)   MODE=$1; shift ;;
  -h|--help|help) usage; exit 0 ;;
esac
# A typo'd mode would otherwise be taken for a hostname and run "all" against
# a host literally named "exce".
case "${1:-}" in
  *@*|*.*) ;;                 # looks like a host: fine
  *) usage; exit 2 ;;         # no host, or a typo'd mode
esac
HOST=$1
PORT=${2:-22}
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
  $V "cd /os0"                   >/dev/null 2>&1; RC=$?; check "cd read-only device root"  0 "$RC"
  $V "cd /vs0"                   >/dev/null 2>&1; RC=$?; check "cd between devices"        0 "$RC"
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
  # shape: one line per directory, child first, size in 1K (6 bytes -> 1), path as typed
  OUT=$($V "du /ux0/data/$T"); WANT=$(printf '1\t/ux0/data/%s/sub\n1\t/ux0/data/%s' "$T" "$T")
  if [ "$OUT" = "$WANT" ]; then check "du prints a line per directory" ok ok
  else printf 'FAIL  du prints a line per directory (got "%s")\n' "$OUT"; FAIL=$((FAIL+1)); fi
  OUT=$($V "du -sh /ux0/data/$T"); WANT=$(printf '1.0K\t/ux0/data/%s' "$T")
  if [ "$OUT" = "$WANT" ]; then check "du -sh prints one total" ok ok
  else printf 'FAIL  du -sh prints one total (got "%s")\n' "$OUT"; FAIL=$((FAIL+1)); fi
  OUT=$($V "du -a /ux0/data/$T" | head -1); WANT=$(printf '1\t/ux0/data/%s/sub/f.txt' "$T")
  if [ "$OUT" = "$WANT" ]; then check "du -a lists files" ok ok
  else printf 'FAIL  du -a lists files (got "%s")\n' "$OUT"; FAIL=$((FAIL+1)); fi
  # cross-device mv: the first candidate device that accepts the write
  MVOK=""
  for DEV2 in uma0 tm0 grw0 ud0 ur0; do
    if $V "mv /ux0/data/$T/sub/f.txt /$DEV2/smoke-$$.txt" >/dev/null 2>&1; then
      MVOK="$DEV2"; break
    fi
  done
  if [ -z "$MVOK" ]; then
    printf 'SKIP  cross-device mv: no second writable device accepted it (plug a USB drive into the Vita)\n'
  else
    # Not just the statuses: the bytes have to arrive and the source has to go,
    # which is what a copy-and-delete mv can get wrong without saying so.
    OUT=$($V "cat /$MVOK/smoke-$$.txt")
    if [ "$OUT" = "hello" ]; then check "mv across devices: bytes arrive (->$MVOK)" ok ok
    else printf 'FAIL  mv across devices: bytes arrive (->$MVOK got "%s")\n' "$OUT"; FAIL=$((FAIL+1)); fi
    $V "cat /ux0/data/$T/sub/f.txt" >/dev/null 2>&1; RC=$?
    check "mv across devices: source removed (->$MVOK)" 1 "$RC"
    $V "rm /$MVOK/smoke-$$.txt" >/dev/null 2>&1
    $V "echo hello > /ux0/data/$T/sub/f.txt" >/dev/null 2>&1
  fi
  # rm -r refuses "." and ".." as typed; resolving "dir/." gives "dir", so only the argument shows it
  $V "rm -r /ux0/data/$T/sub/." >/dev/null 2>&1; RC=$?; check "rm -r dir/. refused" 1 "$RC"
  $V "rm -r /ux0/data/$T/sub/.." >/dev/null 2>&1; RC=$?; check "rm -r dir/.. refused" 1 "$RC"
  $V "ls /ux0/data/$T/sub" >/dev/null 2>&1; RC=$?; check "tree survives rm -r dir/." 0 "$RC"
  $V "rm -r /ux0/data/$T /ux0/data/$T-copy" >/dev/null 2>&1
  $V "ls /ux0/data/$T"  >/dev/null 2>&1; RC=$?; check "rm -r removes the tree"      1 "$RC"
  $V "rm /nosuchfile"   >/dev/null 2>&1; RC=$?; check "rm missing errs honestly"    1 "$RC"

  # --- TASK-015.02: pipelines ----------------------------------------------
  # Segments run one after another in the server; the `|` word must stand alone, like `>`.
  OUT=$($V "echo x | cat" 2>/dev/null);               check "pipe: echo x | cat" x "$OUT"
  OUT=$($V "echo a | cat | cat | cat" 2>/dev/null);   check "pipe: three segments chain" a "$OUT"
  OUT=$($V 'echo "a|b"' 2>/dev/null);                 check "pipe: quoted | stays literal" 'a|b' "$OUT"
  OUT=$($V "ls /ux0 | grep -c data" 2>/dev/null);     check "pipe: ls | grep finds data" 1 "$OUT"
  $V "echo hi | cat > /ux0/data/$T-pipe.txt" >/dev/null 2>&1
  OUT=$($V "cat /ux0/data/$T-pipe.txt" 2>/dev/null);  check "pipe: last segment honours >" hi "$OUT"
  OUT=$($V "echo mid > /ux0/data/$T-pipe.txt | cat" 2>/dev/null)
  check "pipe: redirect on a middle segment wins" "" "$OUT"
  OUT=$($V "cat /ux0/data/$T-pipe.txt | head -n 1" 2>/dev/null); check "pipe: file contents reach the next segment" mid "$OUT"
  $V "echo x | nosuchcommand"  >/dev/null 2>&1; RC=$?; check "pipe: status is the last segment's (-> 127)" 127 "$RC"
  OUT=$($V "nosuchcommand | echo after" 2>&1 | grep -v "closed by remote host")
  check "pipe: unknown command reports once, later segments run" "sh: nosuchcommand: command not found after" "$(printf '%s' "$OUT" | tr '\n' ' ' | sed 's/ $//')"
  check "pipe: cd mid-pipeline leaves the cwd" "$($V pwd 2>/dev/null)" "$($V "cd /ux0 | pwd" 2>/dev/null)"
  OUT=$($V "exit | echo still" 2>/dev/null);          check "pipe: exit mid-pipeline does not end the session" still "$OUT"
  $V "| cat"      >/dev/null 2>&1; RC=$?; check "pipe: leading | is a syntax error (-> 2)" 2 "$RC"
  $V "echo x |"   >/dev/null 2>&1; RC=$?; check "pipe: trailing | is a syntax error (-> 2)" 2 "$RC"
  $V "rm /ux0/data/$T-pipe.txt" >/dev/null 2>&1

  # --- TASK-025: chaining with ; && || -------------------------------------
  # Left to right, equal precedence, looser than `|`; operators are separate unquoted words.
  OUT=$($V 'echo a ; echo b' 2>/dev/null | tr '\n' ' ');      check "chain: ; runs both in order" "a b " "$OUT"
  $V 'false ; true'  >/dev/null 2>&1; RC=$?;                  check "chain: ; status is the last command's (0)" 0 "$RC"
  $V 'true ; false'  >/dev/null 2>&1; RC=$?;                  check "chain: ; status is the last command's (1)" 1 "$RC"
  OUT=$($V 'true && echo yes' 2>/dev/null);                   check "chain: && runs after status 0" yes "$OUT"
  OUT=$($V 'false && echo no' 2>/dev/null);                   check "chain: && skips after nonzero" "" "$OUT"
  $V 'false && echo no' >/dev/null 2>&1; RC=$?;               check "chain: skipped && keeps the failing status" 1 "$RC"
  OUT=$($V 'false || echo yes' 2>/dev/null);                  check "chain: || runs after nonzero" yes "$OUT"
  OUT=$($V 'true || echo no' 2>/dev/null);                    check "chain: || skips after status 0" "" "$OUT"
  $V 'true || echo no' >/dev/null 2>&1; RC=$?;                check "chain: skipped || keeps status 0" 0 "$RC"
  OUT=$($V 'ls /nope && echo yes || echo no' 2>&1 | grep -v 'closed by remote host' | tail -n 1); check "chain: failing && then || runs the || arm" no "$OUT"
  OUT=$($V 'ls /ux0 && echo yes || echo no' 2>/dev/null | tail -n 1); check "chain: passing && then || skips the || arm" yes "$OUT"
  OUT=$($V 'echo x | cat && echo y' 2>/dev/null | tr '\n' ' '); check "chain: operators bind looser than |" "x y " "$OUT"
  OUT=$($V 'uname -a ; pwd' 2>/dev/null | wc -l | tr -d ' ');  check "chain: one-shot uname -a ; pwd prints both" 2 "$OUT"
  $V 'uname -a ; pwd' >/dev/null 2>&1; RC=$?;                 check "chain: one-shot exits with the last status" 0 "$RC"
  OUT=$($V 'echo "a; b"' 2>/dev/null);                        check "chain: ; in double quotes stays literal" 'a; b' "$OUT"
  OUT=$($V "echo 'x && y'" 2>/dev/null);                      check "chain: && in single quotes stays literal" 'x && y' "$OUT"
  OUT=$($V 'echo a;b' 2>/dev/null);                           check "chain: glued ; is just text" 'a;b' "$OUT"
  OUT=$($V "echo a > /ux0/data/$T-chain.txt ; echo b" 2>/dev/null); check "chain: redirect applies to its own command only" b "$OUT"
  OUT=$($V "cat /ux0/data/$T-chain.txt" 2>/dev/null);         check "chain: the redirected command wrote its file" a "$OUT"
  $V "rm /ux0/data/$T-chain.txt" >/dev/null 2>&1
  OUT=$($V 'exit ; echo never' 2>/dev/null);                  check "chain: exit stops the chain" "" "$OUT"
  OUT=$($V 'false ; false ; echo last' 2>/dev/null);          check "chain: ; ignores earlier failures" last "$OUT"
  OUT=$($V 'echo a ; ; echo b' 2>&1 | grep -c -E '^(a|b)$');  check "chain: doubled operator runs nothing" 0 "$OUT"
  $V 'echo a ; ; echo b' >/dev/null 2>&1; RC=$?;              check "chain: doubled operator is a syntax error (-> 2)" 2 "$RC"
  $V 'echo a &&'  >/dev/null 2>&1; RC=$?;                     check "chain: trailing && is a syntax error (-> 2)" 2 "$RC"
  OUT=$($V 'echo a ;' 2>/dev/null);                           check "chain: a trailing ; is accepted, as in sh" a "$OUT"
  $V 'echo a ;' >/dev/null 2>&1; RC=$?;                       check "chain: a trailing ; leaves the status (0)" 0 "$RC"
  $V 'echo a ||'  >/dev/null 2>&1; RC=$?;                     check "chain: trailing || is a syntax error (-> 2)" 2 "$RC"
  $V '&& echo a'  >/dev/null 2>&1; RC=$?;                     check "chain: leading && is a syntax error (-> 2)" 2 "$RC"
  OUT=$($V 'echo a ; | cat' 2>&1 | grep -c -E '^a$');         check "chain: a bad pipe in a later item runs nothing" 0 "$OUT"
  ERR=$($V 'echo a ; ; echo b' 2>&1 | grep -v 'closed by remote host')
  case "$ERR" in
    *"near unexpected token \`;'"*) check "chain: syntax error names the operator" ok ok ;;
    *) check "chain: syntax error names the operator: got \"$ERR\"" ok fail ;;
  esac
  LONG="true"; for d in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do LONG="$LONG ; true"; done
  $V "$LONG" >/dev/null 2>&1; RC=$?;                          check "chain: 17 commands is over the limit (-> 2)" 2 "$RC"
  OUT=$($V "echo ran ; $LONG" 2>&1 | grep -c -E '^ran$');     check "chain: an over-long line runs none of it" 0 "$OUT"
  ERR=$($V "$LONG" 2>&1 | grep -v 'closed by remote host')
  case "$ERR" in
    *"limit 16"*) check "chain: over-long error names the limit" ok ok ;;
    *) check "chain: over-long error names the limit: got \"$ERR\"" ok fail ;;
  esac
  OUT=$($V 'pwd' 2>/dev/null | wc -l | tr -d ' ');            check "chain: the session still works after errors" 1 "$OUT"

  # --- TASK-018: aliases ---------------------------------------------------
  # Per session: every ssh call below is its own session, so each check defines what it uses.
  OUT=$($V 'alias' 2>/dev/null | tr '\n' ' ');                check "alias: a fresh session lists the seeded aliases" "alias ..='cd ..' alias ...='cd ../..' alias l='ls -l' alias la='ls -a' alias ll='ls -la' " "$OUT"
  check "alias: l is ls -l" "$($V 'ls -l /ux0' 2>/dev/null)" "$($V 'l /ux0' 2>/dev/null)"
  OUT=$($V 'la /ux0' 2>/dev/null | wc -l | tr -d ' ');        [ "$OUT" -ge 1 ] && check "alias: la runs" ok ok || check "alias: la runs" ok fail
  OUT=$($V 'cd /ux0/data ; .. ; pwd' 2>/dev/null);            check "alias: .. moves up one" /ux0 "$OUT"
  OUT=$($V 'cd /ux0/data/vitasshd ; ... ; pwd' 2>/dev/null);  check "alias: ... moves up two" /ux0 "$OUT"
  OUT=$($V "alias two='echo a b' ; two" 2>/dev/null);         check "alias: a value with spaces survives quoting" "a b" "$OUT"
  OUT=$($V "alias g='echo hi' ; echo g" 2>/dev/null);         check "alias: an argument naming an alias is untouched" g "$OUT"
  OUT=$($V "alias g='echo hi' ; echo x | g" 2>/dev/null);     check "alias: expands as a pipeline segment's command" hi "$OUT"
  OUT=$($V "alias g='echo hi' ; false || g" 2>/dev/null);     check "alias: expands as a chain item's command" hi "$OUT"
  OUT=$($V "alias s='echo a ; echo b' ; s" 2>/dev/null);      check "alias: a ; inside a value is not re-split" "a ; echo b" "$OUT"
  check "alias: a value may start with its own name" "$($V 'ls -l /ux0' 2>/dev/null)" "$($V "alias ls='ls -l' ; ls /ux0" 2>/dev/null)"
  OUT=$($V "alias echo='echo wins' ; echo x" 2>/dev/null);    check "alias: an alias wins over a builtin" "wins x" "$OUT"
  check "alias: a value starting with another alias expands through it" "$($V 'ls -la /ux0' 2>/dev/null)" "$($V "alias m=ll ; m /ux0" 2>/dev/null)"
  check "alias: a name coming back around reaches the builtin" "$($V 'ls -l /ux0' 2>/dev/null)" "$($V "alias ls='ls -l' ; alias mine=ls ; mine /ux0" 2>/dev/null)"
  $V "alias a1=a2 ; alias a2=a1" >/dev/null 2>&1; RC=$?;       check "alias: closing a ring is refused (-> 1)" 1 "$RC"
  ERR=$($V "alias a1=a2 ; alias a2=a1" 2>&1 | grep -v 'closed by remote host')
  case "$ERR" in
    *"loop of aliases"*) check "alias: the ring refusal is readable" ok ok ;;
    *) check "alias: the ring refusal is readable: got \"$ERR\"" ok fail ;;
  esac
  OUT=$($V "alias a1=a2 ; alias a2=a1 ; alias" 2>/dev/null | grep -c "^alias a2=");    check "alias: the refused definition is not stored" 0 "$OUT"
  OUT=$($V "unalias l ; l /" 2>&1 | grep -v 'closed by remote host' | head -n 1 | cut -c1-30); check "alias: unalias removes it" "sh: l: command not found" "$OUT"
  $V "unalias nosuchalias" >/dev/null 2>&1; RC=$?;             check "alias: unalias of an unknown name exits 1" 1 "$RC"
  ERR=$($V "unalias nosuchalias" 2>&1 | grep -v 'closed by remote host'); check "alias: unalias names the missing alias" "unalias: nosuchalias: not found" "$ERR"
  $V "alias nosuchalias" >/dev/null 2>&1; RC=$?;               check "alias: showing an unknown alias exits 1" 1 "$RC"
  $V "alias 'bad name'=x" >/dev/null 2>&1; RC=$?;              check "alias: an invalid name is refused (-> 1)" 1 "$RC"
  OUT=$($V "alias z='echo made' ; l /ux0 > /ux0/data/$T-alias.txt ; cat /ux0/data/$T-alias.txt" 2>/dev/null | wc -l | tr -d ' ')
  [ "$OUT" -ge 1 ] && check "alias: an alias before > redirects its output" ok ok || check "alias: an alias before > redirects its output" ok fail
  $V "rm /ux0/data/$T-alias.txt" >/dev/null 2>&1
  OUT=$($V "alias a1=x" 2>/dev/null; $V 'alias' 2>/dev/null | grep -c '^alias a1='); check "alias: a definition does not reach another session" 0 "$OUT"
  OUT=$($V "which ll" 2>/dev/null);                            check "alias: which reports an alias" "ll: aliased to ls -la" "$OUT"
  OUT=$($V "help ll" 2>/dev/null);                             check "alias: help reports an alias" 'll is an alias for `ls -la`' "$OUT"
  OUT=$($V "alias ls='ls -l' ; help ls" 2>/dev/null);          check "alias: help says an alias wins over a builtin" 'ls is an alias for `ls -l`; it wins over the builtin of the same name' "$OUT"

  # --- TASK-020: session variables and $NAME expansion ---------------------
  OUT=$($V 'T20=bar ; printenv T20 ; T20=baz ; echo $T20 ; unset T20 ; printenv T20' 2>/dev/null | tr '\n' ' ');   check "var: set, replace and unset" "bar baz " "$OUT"
  OUT=$($V 'X=hi ; echo ${X}there $X.x "[$NOPE]" a$NOPE.b' 2>/dev/null);   check "var: braced form, unset names expand to nothing" "hithere hi.x [] a.b" "$OUT"
  OUT=$($V 'echo a $NOPE b ; echo a "$NOPE" b' 2>/dev/null | tr '\n' '|'); check "var: unquoted unset leaves no word, quoted leaves an empty one" "a b|a  b|" "$OUT"
  OUT=$($V 'S="a   b" ; echo $S ; echo "$S"' 2>/dev/null | tr '\n' '|');   check "var: an unquoted value splits at IFS, a quoted one does not" "a b|a   b|" "$OUT"
  OUT=$($V 'IFS=: ; P=a:b:c ; echo $P ; echo "$P" ; P="a::b" ; echo $P' 2>/dev/null | tr '\n' '|'); check "var: IFS is the separator, and an empty field stays" "a b c|a:b:c|a  b|" "$OUT"
  OUT=$($V 'IFS= ; S="a   b" ; echo $S ; unset IFS ; echo $S' 2>/dev/null | tr '\n' '|'); check "var: an empty IFS does not split, unset IFS is blank, tab and newline" "a   b|a b|" "$OUT"
  OUT=$($V 'S="a   b" ; T=$S ; echo "$T"' 2>/dev/null);                      check "var: an assignment value is not split" "a   b" "$OUT"
  OUT=$($V 'C="echo hi there" ; $C' 2>/dev/null);                            check "var: a value can supply the command and its arguments" "hi there" "$OUT"
  OUT=$($V 'P="/ux0/data/vitassh*" ; echo $P ; echo "$P" ; echo "x"$P' 2>/dev/null | tr '\n' '|');  check "var: an unquoted value globs, a quoted one does not" "/ux0/data/vitassh /ux0/data/vitasshd /ux0/data/vitasshd-update|/ux0/data/vitassh*|x/ux0/data/vitassh*|" "$OUT"
  OUT=$($V "O='>' ; echo hi \$O x ; S=';' ; echo a \$S echo b" 2>/dev/null | tr '\n' '|'); check "var: a value is not a redirect or chain operator" "hi > x|a ; echo b|" "$OUT"
  OUT=$($V "alias g='echo hi' ; C=g ; \$C x" 2>&1 | head -n 1);              check "var: a value is not alias-expanded" "sh: g: command not found" "$OUT"
  OUT=$($V "F=/ux0/data/$T-var.txt ; echo made > \$F ; cat \$F ; rm \$F" 2>/dev/null); check "var: a redirect target may use a variable" made "$OUT"
  OUT=$($V "D=/ux0/data ; cd \$D ; pwd" 2>/dev/null);                        check "var: cd \$D" /ux0/data "$OUT"
  OUT=$($V "echo '\$HOME' \\\$HOME \"\\\$HOME\"" 2>/dev/null);              check "var: single quotes and backslash keep the dollar" '$HOME $HOME $HOME' "$OUT"
  for BAD in HOME PWD USER LOGNAME TMPDIR; do
    ERR=$($V "$BAD=/x" 2>&1 | grep -v 'closed by remote host');            check "var: $BAD is readonly" "sh: $BAD: readonly variable" "$ERR"
  done
  ERR=$($V 'unset HOME' 2>&1 | grep -v 'closed by remote host');            check "var: unset of a readonly name is refused" "unset: HOME: readonly variable" "$ERR"
  OUT=$($V 'HOME=/x ; cd ~ ; pwd' 2>/dev/null);                              [ "$OUT" != "/x" ] && check "var: ~ is unaffected by a refused HOME" ok ok || check "var: ~ is unaffected by a refused HOME" ok fail
  for BAD in 'echo $(date)' 'echo $((1+1))' 'echo ${A:-x}' 'echo $1'; do
    $V "$BAD" >/dev/null 2>&1; RC=$?;                                        check "var: unsupported syntax exits 2 ($BAD)" 2 "$RC"
  done
  ERR=$($V 'echo $(date)' 2>&1 | grep -v 'closed by remote host');           case $ERR in *'near "$("'*) check "var: unsupported syntax names the construct" ok ok ;; *) check "var: unsupported syntax names the construct: got \"$ERR\"" ok fail ;; esac
  for BAD in 'echo ${' 'echo ${}' 'echo ${A'; do
    $V "$BAD" >/dev/null 2>&1; RC=$?;                                        check "var: malformed brace exits 2 ($BAD)" 2 "$RC"
  done
  OUT=$($V 'A=1 echo hi' 2>&1 | grep -v 'closed by remote host');           case $OUT in *"assignment before a command is not supported"*) check "var: an assignment before a command is refused" ok ok ;; *) check "var: an assignment before a command is refused: got \"$OUT\"" ok fail ;; esac
  OUT=$($V 'export E1=one E2=two ; echo $E1 $E2' 2>/dev/null);              check "var: export is assignment" "one two" "$OUT"
  OUT=$($V 'X=1 | cat ; printenv X' 2>/dev/null);                            check "var: an assignment in a pipeline segment is undone" "" "$OUT"
  OUT=$($V 'V1=1 V2=2 V3=3 V4=4 V5=5 V6=6 V7=7 V8=8 V9=9 V10=1 V11=1 V12=1 V13=1 V14=1 V15=1 V16=1 V17=1 V18=1 V19=1 V20=1 V21=1 V22=1 V23=1 V24=1 V25=1 V26=1 V27=1 V28=1 V29=1 V30=1 ; echo alive' 2>&1 | grep -v 'closed by remote host' | tail -n 1); check "var: a full table is refused and the session keeps working" alive "$OUT"
  OUT=$($V "T20ISO=mine" 2>/dev/null; $V 'printenv T20ISO' 2>/dev/null);     check "var: a variable does not reach another session" "" "$OUT"

  # --- TASK-001.08: what `vpk install` accepts -----------------------------
  # Small generated packages (scripts/make_vpk_fixture.py) that are never installed: each is refused
  # before the system installer is called. Installing a real third-party title and replacing an
  # installed one are checked by hand, since they leave an app on the device. A running third-party
  # title cannot be arranged here, because the server is the one foreground app.
  FIX=$(mktemp -d)
  local SSHO="-o BatchMode=yes -o ConnectTimeout=5"
  fixture() { # TITLE [make_vpk_fixture.py options...]: uploads it as /ux0/data/$T-fix.vpk
    local title=$1; shift
    python3 -I "$(dirname "$0")/make_vpk_fixture.py" "$FIX/f.vpk" --title "$title" "$@" &&
      scp -s -q $SSHO -P "$PORT" "$FIX/f.vpk" "$HOST:/ux0/data/$T-fix.vpk" 2>/dev/null
  }
  vpk_out() { $V "$1 /ux0/data/$T-fix.vpk" 2>&1 | grep -v 'closed by remote host'; }
  fixture ZZZZ99999 && OUT=$(vpk_out "vpk list");      case "$OUT" in *"Title ID: ZZZZ99999 (not this app)"*) check "vpk: list reports another title" ok ok ;; *) check "vpk: list reports another title: got \"$OUT\"" ok fail ;; esac
  fixture ROM000070 && OUT=$(vpk_out "vpk list");      case "$OUT" in *"Title ID: ROM000070 (this app, running)"*) check "vpk: list reports this app as running" ok ok ;; *) check "vpk: list reports this app as running: got \"$OUT\"" ok fail ;; esac
  fixture ROM000072 && OUT=$(vpk_out "vpk install");   case "$OUT" in *"bundled updater"*) check "vpk: the updater title is refused" ok ok ;; *) check "vpk: the updater title is refused: got \"$OUT\"" ok fail ;; esac
  $V "vpk install /ux0/data/$T-fix.vpk" >/dev/null 2>&1; RC=$?;     check "vpk: the updater title exits 1" 1 "$RC"
  fixture NPXS99999 && OUT=$(vpk_out "vpk install");   case "$OUT" in *"system app"*) check "vpk: a system title is refused" ok ok ;; *) check "vpk: a system title is refused: got \"$OUT\"" ok fail ;; esac
  $V "ls /ux0/app/NPXS99999" >/dev/null 2>&1; RC=$?;              check "vpk: a refused system title installs nothing" 1 "$RC"
  fixture bad-title && OUT=$(vpk_out "vpk install");   case "$OUT" in *"is not a title ID"*) check "vpk: a malformed title ID is refused" ok ok ;; *) check "vpk: a malformed title ID is refused: got \"$OUT\"" ok fail ;; esac
  fixture ZZZZ99998 --magic XXXX && OUT=$(vpk_out "vpk install"); case "$OUT" in *"SCE magic"*) check "vpk: a bad eboot magic is refused" ok ok ;; *) check "vpk: a bad eboot magic is refused: got \"$OUT\"" ok fail ;; esac
  fixture ZZZZ99997 --eboot-size 100 && OUT=$(vpk_out "vpk install"); case "$OUT" in *"too small"*) check "vpk: an undersized eboot is refused" ok ok ;; *) check "vpk: an undersized eboot is refused: got \"$OUT\"" ok fail ;; esac
  $V "ls /ux0/app/ZZZZ99998" >/dev/null 2>&1; RC=$?;              check "vpk: a refused package is not installed" 1 "$RC"
  OUT=$($V "ls /ux0/data/vitasshd-update" 2>/dev/null | grep -c third); check "vpk: refusals leave no staged directory" 0 "$OUT"
  $V "vpk install -r" >/dev/null 2>&1; RC=$?;                      check "vpk: install -r with no file is a usage error (-> 2)" 2 "$RC"
  $V "vpk install --reinstall" >/dev/null 2>&1; RC=$?;             check "vpk: install --reinstall with no file is a usage error (-> 2)" 2 "$RC"
  $V "vpk list --reinstall /ux0/data/x.vpk" >/dev/null 2>&1; RC=$?;  check "vpk: list takes no --reinstall (-> 2)" 2 "$RC"
  # ROM000069 (the client) is installed: without --reinstall a package for it is refused, and the refusal names the option.
  # Never run this fixture with --reinstall; it would replace the client with a package that is not runnable.
  fixture ROM000069 && OUT=$(vpk_out "vpk install"); case "$OUT" in *"already installed"*"--reinstall"*) check "vpk: an installed title is refused and names --reinstall" ok ok ;; *) check "vpk: an installed title is refused and names --reinstall: got \"$OUT\"" ok fail ;; esac
  $V "vpk install /ux0/data/$T-fix.vpk" >/dev/null 2>&1; RC=$?;     check "vpk: an installed title exits 1 without --reinstall" 1 "$RC"
  $V "rm /ux0/data/$T-fix.vpk" >/dev/null 2>&1
  rm -rf "$FIX"

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
    local D SSHA SSHB SSHC
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

    # AC8: cursor editing -- each case only runs if the edit landed where the
    # cursor was, so the marker line is the command's own output
    printf 'cho smoke-ca\x01e\r' >&3; sleep 1                     # Ctrl-A, insert at start
    printf 'echo smoke-ce\x01\x05X\r' >&3; sleep 1                 # Ctrl-E back to the end
    printf 'echo smoke-lfg\x1b[D\x1b[DZ\r' >&3; sleep 1           # Left twice, insert mid-line
    printf 'echo smoke-rt\x01\x1b[C\x1b[C\x1b[C\x1b[C\x1b[CQ\r' >&3; sleep 1  # Right x5 over "echo "
    printf 'echo smoke-bsZ1\x1b[D\x7f\r' >&3; sleep 1             # backspace mid-line
    printf 'echo smoke-cd\x04\r' >&3; sleep 1                      # Ctrl-D on a non-empty line is a no-op

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

    # AC8: Ctrl-D on an empty line ends the session. The FIFO's write end
    # stays open so the client sees no EOF of its own: only the key can end it.
    mkfifo "$D/c.in"
    ssh -tt -p $PORT -o BatchMode=yes "$HOST" < "$D/c.in" > "$D/c.out" 2>&1 &
    SSHC=$!
    exec 5> "$D/c.in"
    sleep 2
    printf '\x04' >&5; sleep 2
    kill -0 $SSHC 2>/dev/null && check "Ctrl-D on an empty line ended the session" ok fail \
      || check "Ctrl-D on an empty line ended the session" ok ok
    kill $SSHC 2>/dev/null
    exec 5>&-

    # --- grade the transcripts -------------------------------------------
    # The echo alone proves nothing: the server must also have run the completed name, not the typed prefix.
    grep -q "cat sshd.log" "$D/a.out" && ! grep -q "cat: sshd\.:" "$D/a.out" && check "tab completion filled sshd.log" ok ok || check "tab completion filled sshd.log" ok fail
    grep -q "^/ux0" "$D/b.out" && check "session B kept its own cwd (isolation)" ok ok || check "session B kept its own cwd (isolation)" ok fail
    [ "$(grep -c 'smoke-hist-42' "$D/a.out")" -ge 2 ] && check "!! re-ran the command" ok ok || check "!! re-ran the command" ok fail
    grep -q "after-cu" "$D/a.out" && ! grep -q "^GARBAGE" "$D/a.out" && check "Ctrl-U wiped the line" ok ok || check "Ctrl-U wiped the line" ok fail
    grep -q "one three" "$D/a.out" && check "Ctrl-W wiped one word" ok ok || check "Ctrl-W wiped one word" ok fail
    grep -q "^smoke-ca" "$D/a.out" && check "Ctrl-A moved to the start of the line" ok ok || check "Ctrl-A moved to the start of the line" ok fail
    grep -q "^smoke-ceX" "$D/a.out" && check "Ctrl-E moved to the end of the line" ok ok || check "Ctrl-E moved to the end of the line" ok fail
    grep -q "^smoke-lZfg" "$D/a.out" && check "Left arrow moved the cursor, insert mid-line" ok ok || check "Left arrow moved the cursor, insert mid-line" ok fail
    grep -q "^Qsmoke-rt" "$D/a.out" && check "Right arrow moved the cursor" ok ok || check "Right arrow moved the cursor" ok fail
    grep -q "^smoke-bs1" "$D/a.out" && check "backspace deleted before the cursor" ok ok || check "backspace deleted before the cursor" ok fail
    grep -q "^smoke-cd" "$D/a.out" && check "Ctrl-D on a non-empty line did not end the session" ok ok || check "Ctrl-D on a non-empty line did not end the session" ok fail
    grep -q $'\033\[2J' "$D/a.out" && check "Ctrl-L repainted the screen" ok ok || check "Ctrl-L repainted the screen" ok fail
    grep -q '\^C' "$D/a.out" && ! grep -q "^BAD" "$D/a.out" && check "Ctrl-C abandoned the line" ok ok || check "Ctrl-C abandoned the line" ok fail
    grep -q "before-kill" "$D/b.out" && check "session B ran before the kill" ok ok || check "session B ran before the kill" ok fail

    printf '\npty: %d pass, %d fail\n' "$PASS" "$FAIL"
    printf 'full transcripts: %s/a.out %s/b.out\n' "$D" "$D"
    [ "$FAIL" -eq 0 ]
  )
}

# SFTP half: the stock OpenSSH sftp and scp clients in batch mode, graded on
# exit statuses and file contents. sftp -b aborts on the first failing command,
# so each graded operation gets its own call.
run_sftp() {
  # A subshell on purpose: its own counters and its own mktemp dir, removed on exit.
  (
    # ServerAlive* is what ends a session whose server died mid-call; ConnectTimeout
    # only covers the TCP connect.
    local SSHO="-o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
    local REMOTE=/ux0/data/smoke-sftp-$$ LOCAL OUT RC BAD LNK LINES LEFT BGPID OTHER d
    PASS=0; FAIL=0; SKIP=0
    LOCAL=$(mktemp -d)
    trap 'rm -rf "$LOCAL"' EXIT

    local ERRF="$LOCAL/stderr"

    skip() { printf 'SKIP  %s (%s)\n' "$1" "$2"; SKIP=$((SKIP+1)); }
    # Like the shared grader, plus the last client's stderr under a failure, since
    # "255" alone says nothing about why.
    check() {
      if [ "$2" = "$3" ]; then
        printf 'PASS  %-44s\n' "$1"; PASS=$((PASS+1))
      else
        printf 'FAIL  %-44s (%s, want %s)\n' "$1" "$3" "$2"; FAIL=$((FAIL+1))
        [ -s "$ERRF" ] && head -3 "$ERRF" | sed 's/^/      stderr: /'
      fi
    }
    # Prints sftp's stdout, returns its status; stderr goes to $ERRF.
    sftp_run() { printf '%s\n' "$1" | sftp -q $SSHO -P "$PORT" -b - "$HOST" 2>"$ERRF"; }
    # ok when two local files are byte-identical.
    same() { cmp -s "$1" "$2" && echo ok || echo differ; }
    # 1 when the previous command failed, else 0, for checks that want a refusal.
    failed() { [ "$1" -ne 0 ] && echo 1 || echo 0; }

    head -c 6291456 /dev/urandom > "$LOCAL/big.bin"      # larger than any transfer buffer
    printf 'hello vita\n' > "$LOCAL/small.txt"
    : > "$LOCAL/empty.txt"
    # A fixed source mode, so the scp checks do not depend on the umask of the machine running them.
    chmod 644 "$LOCAL/small.txt"

    # --- 002.01: the basics ---------------------------------------------------
    printf 'sftp: opening a session on %s:%s (gives up after ~15s if the server hangs)\n' "$HOST" "$PORT"
    OUT=$(sftp_run "ls /"); RC=$?
    check "sftp connects, ls / (-> 0)" 0 "$RC"
    # Every "refused" check below passes when nothing answers, so stop here.
    if [ "$RC" -ne 0 ]; then
      printf '\nsftp: cannot open an sftp session on %s:%s, aborting\n' "$HOST" "$PORT"
      return 1
    fi
    case "$OUT" in *ux0*) check "ls / lists ux0" ok ok ;; *) check "ls / lists ux0" ok fail ;; esac

    OUT=$(sftp_run "pwd"); RC=$?
    check "pwd (-> 0)" 0 "$RC"
    echo "      cwd: $OUT"

    sftp_run "mkdir $REMOTE" >/dev/null; check "mkdir scratch (-> 0)" 0 $?

    sftp_run "put $LOCAL/big.bin $REMOTE/big.bin" >/dev/null; check "put 6 MB (-> 0)" 0 $?
    sftp_run "get $REMOTE/big.bin $LOCAL/big.back" >/dev/null; check "get 6 MB (-> 0)" 0 $?
    check "6 MB round trip identical" ok "$(same "$LOCAL/big.bin" "$LOCAL/big.back")"

    sftp_run "put $LOCAL/empty.txt $REMOTE/empty.txt" >/dev/null; check "put empty file (-> 0)" 0 $?
    sftp_run "get $REMOTE/empty.txt $LOCAL/empty.back" >/dev/null
    check "empty file round trip identical" ok "$(same "$LOCAL/empty.txt" "$LOCAL/empty.back")"

    OUT=$(sftp_run "ls -1 $REMOTE")
    case "$OUT" in *.part*) check "no .part file left after put" ok fail ;; *) check "no .part file left after put" ok ok ;; esac

    # TASK-002.01 AC6: iteration on a device root and on an empty directory terminates.
    OUT=$(sftp_run "ls -1 /ux0"); RC=$?
    check "ls a device root terminates (-> 0)" 0 "$RC"
    case "$OUT" in *data*) check "device root lists data" ok ok ;; *) check "device root lists data" ok fail ;; esac
    sftp_run "mkdir $REMOTE/empty" >/dev/null
    OUT=$(sftp_run "ls -1 $REMOTE/empty"); RC=$?
    check "ls an empty directory terminates (-> 0)" 0 "$RC"
    LEFT=$(printf '%s\n' "$OUT" | grep -v -e '^\.\{1,2\}$' -e '^$' -e '^sftp> ' | wc -l | tr -d ' ')
    check "empty directory lists no entries" 0 "$LEFT"

    # TASK-002.01 AC7: an upload cut off mid-transfer leaves nothing under the name or as .part.
    head -c 67108864 /dev/urandom > "$LOCAL/huge.bin"
    ( printf 'put %s %s/interrupted.bin\n' "$LOCAL/huge.bin" "$REMOTE" |
        sftp -q $SSHO -P "$PORT" -b - "$HOST" >/dev/null 2>&1 ) &
    BGPID=$!
    disown "$BGPID" 2>/dev/null   # no "Killed" job message from the shell
    sleep 1
    # Without this the check below could pass because the upload never started.
    OUT=$(sftp_run "ls -1 $REMOTE")
    case "$OUT" in
      *interrupted.bin.part*) check "upload in flight shows a .part file" ok ok ;;
      *) check "upload in flight shows a .part file" ok "no .part file" ;;
    esac
    pkill -KILL -P "$BGPID" 2>/dev/null; kill -KILL "$BGPID" 2>/dev/null
    sleep 4    # the server notices the closed connection and tears the session down
    OUT=$(sftp_run "ls -1 $REMOTE")
    case "$OUT" in
      *interrupted.bin*) check "interrupted upload leaves no partial file" ok "left: $(printf '%s' "$OUT" | grep interrupted | tr '\n' ' ')" ;;
      *) check "interrupted upload leaves no partial file" ok ok ;;
    esac

    OUT=$(sftp_run "ls -1 /ux0/data/does-not-exist"); RC=$?
    check "ls missing dir (-> nonzero)" 1 "$(failed "$RC")"

    # --- 002.02: attributes ---------------------------------------------------
    OUT=$(sftp_run "ls -ln $REMOTE")
    LINES=$(printf '%s\n' "$OUT" | awk 'NF >= 9' | wc -l | tr -d ' ')
    check "ls -ln lists the scratch files (>= 2)" ok "$([ "$LINES" -ge 2 ] && echo ok || echo "$LINES")"
    BAD=$(printf '%s\n' "$OUT" | awk 'NF >= 9 && ($3 != 0 || $4 != 0)' | wc -l | tr -d ' ')
    check "ls -ln: every uid and gid is 0" 0 "$BAD"
    LNK=$(printf '%s\n' "$OUT" | awk 'substr($1,1,1) == "l"' | wc -l | tr -d ' ')
    check "ls -ln: no symlink entries" 0 "$LNK"
    OUT=$(sftp_run "ls -ln /ux0/data" | awk 'NF >= 9 && substr($1,1,1) == "l"' | wc -l | tr -d ' ')
    check "ls -ln /ux0/data: no symlink entries" 0 "$OUT"

    sftp_run "put $LOCAL/small.txt $REMOTE/small.txt" >/dev/null
    sftp_run "chmod 600 $REMOTE/small.txt" >/dev/null; RC=$?
    check "chmod refused, not faked (-> nonzero)" 1 "$(failed "$RC")"
    sftp_run "chown 1000 $REMOTE/small.txt" >/dev/null; RC=$?
    check "chown refused, not faked (-> nonzero)" 1 "$(failed "$RC")"
    sftp_run "-chmod 600 $REMOTE/small.txt
ls $REMOTE" >/dev/null
    check "'-' prefixed failure does not abort the batch" 0 $?

    # --- 002.02 AC4: clients that send attributes along with the data ---------
    sftp_run "put -p $LOCAL/small.txt $REMOTE/p.txt" >/dev/null; check "sftp put -p completes (-> 0)" 0 $?
    scp -s $SSHO -P "$PORT" "$LOCAL/small.txt" "$HOST:$REMOTE/scp1.txt" 2>"$ERRF"
    check "scp -s upload completes (-> 0)" 0 $?
    # scp exits 0 and only prints a warning when its fsetstat is refused, so grade stderr too.
    check "scp -s upload prints no fsetstat warning" 0 "$(grep -c fsetstat "$ERRF")"
    scp -s -q $SSHO -P "$PORT" "$HOST:$REMOTE/scp1.txt" "$LOCAL/scp1.back" 2>"$ERRF"
    check "scp -s download completes (-> 0)" 0 $?
    check "scp round trip identical" ok "$(same "$LOCAL/small.txt" "$LOCAL/scp1.back")"
    scp -s -p -q $SSHO -P "$PORT" "$LOCAL/small.txt" "$HOST:$REMOTE/scp2.txt" 2>"$ERRF"
    # -p sends file times, which the Vita cannot honor, so a nonzero exit is the
    # designed answer; the line records it without failing the run.
    skip "scp -s -p upload" "exit $?, times cannot be honoured by design"

    # --- 002.02 AC5: rename ---------------------------------------------------
    sftp_run "rename $REMOTE/small.txt $REMOTE/renamed.txt" >/dev/null; check "rename within a device (-> 0)" 0 $?
    sftp_run "get $REMOTE/renamed.txt $LOCAL/renamed.back" >/dev/null
    check "renamed file has the content" ok "$(same "$LOCAL/small.txt" "$LOCAL/renamed.back")"
    sftp_run "ls $REMOTE/small.txt" >/dev/null; RC=$?
    check "old name is gone (-> nonzero)" 1 "$(failed "$RC")"

    sftp_run "put $LOCAL/big.bin $REMOTE/victim.bin" >/dev/null
    sftp_run "rename $REMOTE/renamed.txt $REMOTE/victim.bin" >/dev/null; check "rename onto existing file replaces it (posix-rename)" 0 $?
    sftp_run "get $REMOTE/victim.bin $LOCAL/victim.back" >/dev/null
    check "replaced file has the new content" ok "$(same "$LOCAL/small.txt" "$LOCAL/victim.back")"

    sftp_run "mkdir $REMOTE/d1
mkdir $REMOTE/d2" >/dev/null
    sftp_run "rename $REMOTE/d1 $REMOTE/d2" >/dev/null; RC=$?
    check "rename onto a directory refused (-> nonzero)" 1 "$(failed "$RC")"

    OTHER=
    for d in uma0 imc0 xmc0; do
      case "$(sftp_run "ls /")" in *"$d"*) OTHER=$d; break ;; esac
    done
    if [ -n "$OTHER" ]; then
      sftp_run "rename $REMOTE/victim.bin /$OTHER/smoke-xdev.bin" >/dev/null; RC=$?
      check "cross-device rename refused (-> nonzero)" 1 "$(failed "$RC")"
      sftp_run "ls $REMOTE/victim.bin" >/dev/null
      check "source still present after refusal" 0 $?
      sftp_run "ls /$OTHER/smoke-xdev.bin" >/dev/null; RC=$?
      check "no copy left on the other device" 1 "$(failed "$RC")"
    else
      skip "cross-device rename refused" "no second writable device in ls /"
    fi
    sftp_run "rename $REMOTE/victim.bin /os0/smoke.bin" >/dev/null; RC=$?
    check "rename onto read-only device refused (-> nonzero)" 1 "$(failed "$RC")"

    # --- TASK-002.03: real-client transfers, checksummed and timed --------------
    local X=$REMOTE/x XL="$LOCAL/x" VPK
    VPK="$(dirname "$0")/../build/ssh_server_vita.vpk"
    mkdir -p "$XL/tree/a/b/c" "$XL/tree/empty"
    head -c 12582912 /dev/urandom > "$LOCAL/over10.bin"   # AC1: over 10 MB
    head -c 3145728 /dev/urandom > "$XL/tree/a/b/c/deep.bin"
    head -c 1 /dev/urandom > "$XL/tree/one.bin"
    : > "$XL/tree/a/zero.bin"
    head -c 70000 /dev/urandom > "$XL/tree/a/b/odd.bin"   # not a multiple of any block size
    # sha, timed and rate (the AC4 THROUGHPUT line) come from the shared library.
    . "$(dirname "$0")/transfer_lib.sh"
    # Name, size and checksum of every file under a directory, for comparing two trees.
    tree_sums() { ( cd "$1" && find . -type f | LC_ALL=C sort | while read -r f; do printf '%s %s %s\n' "$f" "$(wc -c < "$f" | tr -d ' ')" "$(sha "$f")"; done; ( find . -type d | LC_ALL=C sort ) ); }

    sftp_run "mkdir $X" >/dev/null; check "mkdir transfer scratch (-> 0)" 0 $?

    # AC1: scp -s of a file over 10 MB, both directions, sha256
    timed scp -s -q $SSHO -P "$PORT" "$LOCAL/over10.bin" "$HOST:$X/over10.bin" 2>"$ERRF"
    check "scp -s upload 12 MB (-> 0)" 0 $?
    rate scp-up over10.bin 12582912
    timed scp -s -q $SSHO -P "$PORT" "$HOST:$X/over10.bin" "$LOCAL/over10.back" 2>"$ERRF"
    check "scp -s download 12 MB (-> 0)" 0 $?
    rate scp-down over10.bin 12582912
    check "12 MB scp sha256 identical both ways" "$(sha "$LOCAL/over10.bin")" "$(sha "$LOCAL/over10.back" 2>/dev/null)"

    # AC4: sftp throughput on the same file
    timed sftp_run "put $LOCAL/over10.bin $X/over10s.bin" >/dev/null; check "sftp put 12 MB (-> 0)" 0 $?
    rate sftp-put over10.bin 12582912
    timed sftp_run "get $X/over10s.bin $LOCAL/over10s.back" >/dev/null; check "sftp get 12 MB (-> 0)" 0 $?
    rate sftp-get over10.bin 12582912
    check "12 MB sftp sha256 identical both ways" "$(sha "$LOCAL/over10.bin")" "$(sha "$LOCAL/over10s.back" 2>/dev/null)"

    # AC2: recursive put and get reproduce a tree
    sftp_run "put -r $XL/tree $X/tree" >/dev/null; check "sftp put -r tree (-> 0)" 0 $?
    mkdir -p "$LOCAL/treeback"
    sftp_run "get -r $X/tree $LOCAL/treeback/tree" >/dev/null; check "sftp get -r tree (-> 0)" 0 $?
    check "tree names, sizes, sha256 identical" "$(tree_sums "$XL/tree")" "$(tree_sums "$LOCAL/treeback/tree" 2>/dev/null)"

    # AC7: a VPK in one command, byte-identical afterwards
    if [ -f "$VPK" ]; then
      timed scp -s -q $SSHO -P "$PORT" "$VPK" "$HOST:$X/app.vpk" 2>"$ERRF"
      check "scp -s put build VPK (-> 0)" 0 $?
      rate scp-up "$(basename "$VPK")" "$(wc -c < "$VPK" | tr -d ' ')"
      scp -s -q $SSHO -P "$PORT" "$HOST:$X/app.vpk" "$LOCAL/app.vpk.back" 2>"$ERRF"
      check "VPK lands byte-identical (cmp)" ok "$(same "$VPK" "$LOCAL/app.vpk.back")"
      # exec output past 64 KB was cut off when the session was torn down with replies still queued.
      ssh -p "$PORT" $SSHO "$HOST" "cat $X/app.vpk" > "$LOCAL/app.vpk.cat" 2>"$ERRF"
      check "exec cat of the VPK is not cut off" ok "$(same "$VPK" "$LOCAL/app.vpk.cat")"
    else
      skip "VPK put" "build/ssh_server_vita.vpk missing, run task build"
    fi

    # TASK-002.03.02: exec output is paced by the client's window, so a 12 MB cat must arrive whole.
    ssh -p "$PORT" $SSHO "$HOST" "cat $X/over10s.bin" > "$LOCAL/over10.cat" 2>"$ERRF"
    check "exec cat of 12 MB arrives byte-identical" ok "$(same "$LOCAL/over10.bin" "$LOCAL/over10.cat")"
    # A reader that stops reading must not hold the server: it drops that session after ~30 s,
    # then answers the next one. The pipe to sleep keeps ssh's stdout full without closing it.
    ssh -p "$PORT" $SSHO "$HOST" "cat $X/over10s.bin" 2>"$ERRF" | sleep 45
    ssh -p "$PORT" $SSHO "$HOST" "pwd" >/dev/null 2>&1; check "server answers after a reader stalled (-> 0)" 0 $?

    # AC3: the client must be told why. One session runs every refused operation (a "-"
    # prefix keeps the batch going), so there is no exit status per command: a refusal is
    # the client printing text after the echoed command, and a bare "Failure" is graded
    # as unhelpful. The text is printed so it can be reviewed.
    local OPS=("put $LOCAL/small.txt /os0/smoke.txt" "mkdir /os0/smoke" "rm /os0/smoke.txt"
               "get /ux0/data/does-not-exist $LOCAL/nope"
               "put $LOCAL/small.txt /ux0/data/does-not-exist/x.txt"
               "ls /nodevice/x" "rmdir /ux0/data/does-not-exist") op MSG
    OUT=$(for op in "${OPS[@]}"; do printf -- '-%s\n' "$op"; done |
      sftp -q $SSHO -P "$PORT" -b - "$HOST" 2>&1)
    for op in "${OPS[@]}"; do
      # The echoed "sftp> cmd" line, then everything up to the next echo.
      MSG=$(printf '%s\n' "$OUT" | awk -v c="$op" '
        /^sftp> / { line = $0; sub(/^sftp> -?/, "", line); on = (line == c); next }
        on && !/^Connection to / { printf "%s ", $0 }')
      printf '      %s -> %s\n' "$(printf '%s' "$op" | sed "s|$LOCAL|\$LOCAL|g")" "$MSG"
      check "refused: $(printf '%s' "$op" | sed "s|$LOCAL/||g" | cut -c1-40)" ok "$([ -n "$MSG" ] && echo ok || echo silent)"
      case "$MSG" in
        *": Failure"*|*"Unknown error"*) check "  message names the cause: ${op%% *}" ok "generic: $MSG" ;;
        *) check "  message names the cause: ${op%% *}" ok ok ;;
      esac
    done

    # AC8: legacy scp has no scp binary behind the shell; it must fail fast, not hang.
    timed perl -e 'alarm 20; exec @ARGV' scp -O -q $SSHO -P "$PORT" "$LOCAL/small.txt" "$HOST:$X/legacy.txt" 2>"$ERRF"
    RC=$?
    printf '      scp -O -> exit %s in %ss: %s\n' "$RC" "$ELAPSED" "$(tr '\n' ' ' < "$ERRF" | cut -c1-160)"
    check "scp -O fails, not hangs (-> nonzero, not 142)" ok "$([ "$RC" -ne 0 ] && [ "$RC" -ne 142 ] && echo ok || echo "$RC")"
    check "scp -O prints a message" ok "$([ -s "$ERRF" ] && echo ok || echo silent)"

    # AC9: two sessions transferring at once do not corrupt each other
    head -c 8388608 /dev/urandom > "$LOCAL/cc1.bin"; head -c 8388608 /dev/urandom > "$LOCAL/cc2.bin"
    scp -s -q $SSHO -P "$PORT" "$LOCAL/cc1.bin" "$HOST:$X/cc1.bin" 2>"$LOCAL/cc1.err" & BGPID=$!
    scp -s -q $SSHO -P "$PORT" "$LOCAL/cc2.bin" "$HOST:$X/cc2.bin" 2>"$LOCAL/cc2.err"; RC=$?
    wait "$BGPID"; check "two concurrent scp uploads (-> 0, 0)" "0 0" "$? $RC"
    scp -s -q $SSHO -P "$PORT" "$HOST:$X/cc1.bin" "$LOCAL/cc1.back" 2>"$ERRF" & BGPID=$!
    scp -s -q $SSHO -P "$PORT" "$HOST:$X/cc2.bin" "$LOCAL/cc2.back" 2>"$ERRF"; RC=$?
    wait "$BGPID"; check "two concurrent scp downloads (-> 0, 0)" "0 0" "$? $RC"
    check "concurrent transfer 1 intact" ok "$(same "$LOCAL/cc1.bin" "$LOCAL/cc1.back")"
    check "concurrent transfer 2 intact" ok "$(same "$LOCAL/cc2.bin" "$LOCAL/cc2.back")"

    # The shell's rm -r removes the tree that sftp has no command for.
    ssh -p "$PORT" $SSHO "$HOST" "rm -r $X" >/dev/null 2>"$ERRF"; check "rm -r transfer scratch (-> 0)" 0 $?

    # What the stock clients cannot reach: a refused subsystem followed by a good one on
    # the same channel (TASK-002.01 AC5) and SETSTAT with times (TASK-002.02 AC1).
    if command -v uv >/dev/null 2>&1; then
      OUT=$(uv run --quiet "$(dirname "$0")/sftp_probe.py" "$HOST" "$PORT" 2>&1)
      printf '%s\n' "$OUT"
      PASS=$((PASS + $(printf '%s\n' "$OUT" | grep -c '^PASS')))
      FAIL=$((FAIL + $(printf '%s\n' "$OUT" | grep -c '^FAIL')))
    else
      skip "scripts/sftp_probe.py" "uv is not installed"
    fi

    # --- cleanup, then 002.01 AC4: the server is still healthy ----------------
    sftp_run "rm $REMOTE/victim.bin
rm $REMOTE/big.bin
rm $REMOTE/empty.txt
-rm $REMOTE/p.txt
-rm $REMOTE/scp1.txt
-rm $REMOTE/scp2.txt
-rm $REMOTE/renamed.txt
-rm $REMOTE/interrupted.bin
-rm $REMOTE/interrupted.bin.part
rmdir $REMOTE/empty
rmdir $REMOTE/d1
rmdir $REMOTE/d2
rmdir $REMOTE" >/dev/null
    check "rm and rmdir clean up (-> 0)" 0 $?

    ssh -p "$PORT" $SSHO "$HOST" "pwd" >/dev/null 2>&1; check "shell session still works afterwards (-> 0)" 0 $?
    sftp_run "pwd" >/dev/null; check "a second sftp session still opens (-> 0)" 0 $?

    printf '\nsftp: %d pass, %d fail, %d skip\n' "$PASS" "$FAIL" "$SKIP"
    [ "$FAIL" -eq 0 ]
  )
}

case "$MODE" in
  exec) run_exec ;;
  pty)  run_pty ;;
  sftp) run_sftp ;;
  all)
    run_exec; RC=$?
    run_pty;  RC=$((RC + $?))
    run_sftp; RC=$((RC + $?))
    if [ "$RC" = 0 ]; then
      printf '\nall: every half passed\n'
      exit 0
    fi
    printf '\nall: failures above\n'
    exit 1
    ;;
esac
