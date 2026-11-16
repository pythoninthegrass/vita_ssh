# Timing helpers shared by smoke.sh and throughput.sh; sourced, not run.

# sha256 of a file, whichever tool the host has.
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

# Runs a command, leaving its wall time in ELAPSED; perl because macOS date has no %N.
timed() {
  local t0 rc
  t0=$(perl -MTime::HiRes=time -e 'print time')
  "$@"; rc=$?
  ELAPSED=$(perl -MTime::HiRes=time -e "printf '%.2f', time - $t0")
  return $rc
}

# One parseable line per transfer, so a later run can be diffed against this one.
# Fields: THROUGHPUT date label file bytes "bytes" seconds "s" rate "MB/s" client=...
rate() { printf 'THROUGHPUT %s %s %s %s bytes %s s %s MB/s client=%s\n' "$(date +%Y-%m-%d)" "$1" "$2" "$3" "$ELAPSED" \
  "$(awk -v b="$3" -v s="$ELAPSED" 'BEGIN { printf "%.2f", b / s / 1048576 }')" "$(ssh -V 2>&1 | cut -d, -f1)"; }
