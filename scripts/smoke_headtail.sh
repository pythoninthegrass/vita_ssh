#!/usr/bin/env bash
# TASK-016.02 device checks for head and tail, over real ssh exec sessions.
#
# Usage: scripts/smoke_headtail.sh any@<vita-ip> [port]      (port defaults to 22)
#
# Run it from a real terminal on the LAN: the authoring agent's sandbox cannot
# reach the device. It grades stdout bytes and exit statuses, so it needs no
# tty, and it removes its scratch when done.
set -u
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
HOST="${1:?usage: $0 user@host [port]}"
PORT="${2:-22}"
SSH=(ssh -p "$PORT" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 "$HOST")
D=/ux0/data/ht-$$
pass=0; fail=0

run() { "${SSH[@]}" "$1" 2>/dev/null; }
code() { "${SSH[@]}" "$1" >/dev/null 2>&1; echo $?; }

check() { # check LABEL EXPECTED ACTUAL
  if [ "$2" = "$3" ]; then pass=$((pass+1)); printf 'PASS %s\n' "$1"
  else fail=$((fail+1)); printf 'FAIL %s\n       want=%q\n       got =%q\n' "$1" "$2" "$3"; fi
}

"${SSH[@]}" "mkdir -p $D" >/dev/null 2>&1

# The fixture, built by echo so its bytes are known: 12 numbered lines plus three
# cases where a tool drifts -- a blank line, an unterminated last line, and a
# file whose only content is a newline.
"${SSH[@]}" "echo ok > $D/f12" >/dev/null 2>&1  # smoke: is the channel alive at all
i=1; while [ $i -le 12 ]; do "${SSH[@]}" "echo L$i >> $D/f12" >/dev/null 2>&1; i=$((i+1)); done
"${SSH[@]}" "printf 'aa\n\nbaa' > $D/f2" >/dev/null 2>&1
"${SSH[@]}" "printf 'a\n\n' > $D/f3" >/dev/null 2>&1
"${SSH[@]}" "printf 'x' > $D/f4" >/dev/null 2>&1
"${SSH[@]}" "printf '' > $D/f5" >/dev/null 2>&1
# --- AC1 head -------------------------------------------------------------
check "AC1 head default is 10 lines"  "L1
L2
L3
L4
L5
L6
L7
L8
L9
L10"  "$(run "head $D/f12")"
check "AC1 head -n 2"  "L1
L2"  "$(run "head -n 2 $D/f12")"
check "AC1 head -n2 attached"  "L1
L2"  "$(run "head -n2 $D/f12")"
check "AC1 head -n 0 prints nothing"  ""  "$(run "head -n 0 $D/f12")"
check "AC1 head -n 0 exit 0"  "0"  "$(code "head -n 0 $D/f12")"
check "AC1 head -c 4"  "L1
L"  "$(run "head -c 4 $D/f12")"
check "AC1 head -n 99 gives all"  "$(run "cat $D/f12")"  "$(run "head -n 99 $D/f12")"

# --- AC2 tail -------------------------------------------------------------
check "AC2 tail default is last 10"  "L3
L4
L5
L6
L7
L8
L9
L10
L11
L12"  "$(run "tail $D/f12")"
check "AC2 tail -n 2"  "L11
L12"  "$(run "tail -n 2 $D/f12")"
check "AC2 tail -n 0 prints nothing"  ""  "$(run "tail -n 0 $D/f12")"
check "AC2 tail -c 4"  "L12"  "$(run "tail -c 4 $D/f12")"
check "AC2 tail -n 99 gives all"  "$(run "cat $D/f12")"  "$(run "tail -n 99 $D/f12")"
check "AC2 tail of an unterminated last line"  "baa"  "$(run "tail -n 1 $D/f2")"
check "AC2 tail -n 2 keeps the blank line"  "
baa"  "$(run "tail -n 2 $D/f2")"
check "AC2 tail -n 1 of 'a<newline><newline>' is the blank line"  ""  "$(run "tail -n 1 $D/f3" | tr -d '\n')"
check "AC2 tail of a one-byte file"  "x"  "$(run "tail -n 1 $D/f4")"
check "AC2 tail of an empty file"  ""  "$(run "tail -n 5 $D/f5")"
check "AC2 tail -n 20 of a 12-line file is all of it"  "$(run "cat $D/f12")"  "$(run "tail -n 20 $D/f12")"

# --- AC3 stdin ------------------------------------------------------------
check "AC3 head reads redirected stdin"  "L1
L2"  "$(run "head -n 2 < $D/f12")"
check "AC3 tail reads redirected stdin"  "L11
L12"  "$(run "tail -n 2 < $D/f12")"
check "AC3 head -c reads redirected stdin"  "L1
L"  "$(run "head -c 4 < $D/f12")"

# --- AC4 several files, with headers --------------------------------------
check "AC4 headers between files"  "==> $D/f4 <==
x
==> $D/f2 <==
aa

baa"  "$(run "head -n 2 $D/f4 $D/f2")"
check "AC4 no header for a single file"  "aa"  "$(run "head -n 1 $D/f2")"

# --- AC5 missing file -----------------------------------------------------
check "AC5 missing file names the reason"  "head: $D/nope: No such file or directory" \
  "$("${SSH[@]}" "head $D/nope" 2>&1 >/dev/null | tail -1)"
check "AC5 missing file exits nonzero"  "1"  "$(code "head $D/nope")"
check "AC5 later files still printed"  "x"  "$(run "head -n 1 $D/nope $D/f4 | tail -n 1")"
check "AC5 unreadable run exits nonzero"  "1"  "$(code "head $D/nope $D/f4")"

# --- AC6 -f refused, not blocking -----------------------------------------
check "AC6 tail -f is refused"  "tail: $D/f12: following a file is not supported" \
  "$("${SSH[@]}" "tail -f $D/f12" 2>&1 >/dev/null | tail -1)"
check "AC6 tail -f exits nonzero"  "1"  "$(code "tail -f $D/f12")"
# The whole point of the refusal: the server still answers immediately after.
check "AC6 server still alive after -f"  "pong"  "$(run "echo pong")"
check "AC6 head -f is refused as an illegal option"  "head: illegal option -- f" \
  "$("${SSH[@]}" "head -f $D/f12" 2>&1 >/dev/null | tail -1)"
check "AC6 tail -f on stdin is refused"  "tail: (standard input): cannot follow standard input" \
  "$("${SSH[@]}" "tail -f < $D/f12" 2>&1 >/dev/null | tail -1)"

# --- AC7 size bound -------------------------------------------------------
# Each check below runs one command per ssh session, so a hung command shows up as a
# stalled run rather than a pass; that is the only grade -f can get on a device.
# -n cannot exceed the file, so the bound is reached through -c, which takes the
# operand as a count. 16 MB + 1 is SH_HT_SIZE_MAX + 1.
check "AC7 tail -c past the bound is refused"  "1"  "$(code "tail -c 16777217 $D/f12")"
check "AC7 the bound is named"  "tail: $D/f12: file too large" \
  "$("${SSH[@]}" "tail -c 16777217 $D/f12" 2>&1 >/dev/null | tail -1)"
check "AC7 a huge -n is answered, not hung"  "$(run "cat $D/f12")"  "$(run "tail -n 999999999 $D/f12")"

# --- bounds and honesty ---------------------------------------------------
# A line over SH_HT_LINE_MAX, built locally and written in one argument.
BIG=$(/usr/bin/printf 'a%.0s' $(seq 1 4200))
"${SSH[@]}" "echo $BIG > $D/wide" >/dev/null 2>&1
check "bound: a long line is reported, not truncated"  "head: $D/wide: line too long (limit 4096 bytes)" \
  "$("${SSH[@]}" "head -n 2 $D/wide" 2>&1 >/dev/null | tail -1)"
check "bound: a long line exits nonzero"  "1"  "$(code "head -n 2 $D/wide")"
check "bound: tail reports the same line bound"  "tail: $D/wide: line too long (limit 4096 bytes)" \
  "$("${SSH[@]}" "tail -n 1 $D/wide" 2>&1 >/dev/null | tail -1)"
# The 4096-byte line itself must still print: the bound is over it, not at it. There is
# no wc yet (TASK-016.03), so the length is counted here from the bytes that come back.
OK4096=$(/usr/bin/printf 'b%.0s' $(seq 1 4096))
"${SSH[@]}" "echo $OK4096 > $D/ok4096" >/dev/null 2>&1
check "bound: a 4096-byte line prints"  "4097"  "$(run "head -n 1 $D/ok4096" | /usr/bin/awk '{print length($0)+1}')"

# --- usage and table rows -------------------------------------------------
check "help head"  "head [-n N | -c N] [FILE] -- first N lines (10) or N bytes of a file or stdin"  "$(run "help head")"
check "help tail"  "tail [-n N | -c N] [FILE] -- last N lines (10) or N bytes of a file or stdin"  "$(run "help tail")"
check "which head"  "head: shell builtin"  "$(run "which head")"
check "which tail"  "tail: shell builtin"  "$(run "which tail")"
check "bad option is 2"  "2"  "$(code "head -z $D/f12")"
check "bad count is 2"  "2"  "$(code "head -n x $D/f12")"
check "a directory is Is a directory"  "1"  "$(code "head $D")"
check "-- ends the options"  "L1"  "$(run "head -n 1 -- $D/f12")"

# --- redirect still works through the sink --------------------------------
"${SSH[@]}" "head -n 3 $D/f12 > $D/out" >/dev/null 2>&1
check "output redirects to a file"  "L1
L2
L3"  "$(run "cat $D/out")"

"${SSH[@]}" "rm -rf $D" >/dev/null 2>&1
echo "----"
echo "head/tail device checks: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
