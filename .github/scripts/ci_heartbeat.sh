#!/usr/bin/env bash
# CI diagnostic heartbeat.
#
# GitHub Actions gives no progress output at all for the "Set up Zephyr"
# composite action while it is downloading/unpacking the SDK -- when that
# step goes silent for 20-60+ minutes there is no way, after the fact, to
# tell "still working" from "wedged" from "just slow this run". This script
# is started as a DETACHED background process by a step BEFORE the step
# being watched, and stopped (SIGTERM) by an `if: always()` step AFTER it,
# so its log covers exactly the silent window regardless of whether that
# step ultimately succeeds, fails, or times out.
#
# Every $INTERVAL seconds it appends one block to $LOG containing:
#   - a UTC timestamp and elapsed seconds since the heartbeat started
#   - free memory (free -h)
#   - disk space on / (df -h)
#   - the runner's own build-tool processes (west, cmake, ninja, gcc/ld,
#     the arm-zephyr-eabi toolchain, python3, ccache) with PID, state, and
#     CPU% -- so a genuine hang (process present, CPU 0%, state D/S) is
#     distinguishable from active work (CPU>0%, state R) or from a runner
#     that simply never started the process at all.
#
# Never prints environment variables or any other value that could contain
# a secret -- only timestamps, resource counters, and process metadata for
# a fixed allow-list of tool names.
set -uo pipefail

LOG="${1:?usage: ci_heartbeat.sh <log-file> [interval-seconds]}"
INTERVAL="${2:-60}"

mkdir -p "$(dirname "$LOG")"
: > "$LOG"
echo "heartbeat started: $(date -u +%FT%TZ) (interval ${INTERVAL}s)" >> "$LOG"
START_TS=$(date +%s)

while true; do
  sleep "$INTERVAL"
  NOW_TS=$(date +%s)
  ELAPSED=$((NOW_TS - START_TS))
  {
    echo "---- $(date -u +%FT%TZ) (elapsed ${ELAPSED}s) ----"
    echo "# memory"
    free -h 2>/dev/null || echo "(free unavailable)"
    echo "# disk (/)"
    df -h / 2>/dev/null || echo "(df unavailable)"
    echo "# build-tool processes"
    # Match on full command lines (args, not the 15-char-truncated comm
    # field), with the tool name anchored to a path-separator or
    # whitespace boundary on both sides, so this can't false-positive on
    # an unrelated process whose name merely contains/ends in the same
    # letters (e.g. a kernel worker thread ending in "...ld").
    { ps -eo pid,ppid,stat,pcpu,etime,args \
        | grep -E '(^|[[:space:]/])(west|cmake|ninja|[a-z0-9_.+-]*-?gcc|[a-z0-9_.+-]*-?ld(\.(bfd|gold|lld))?|arm-zephyr-eabi-[a-z]+|python3|ccache)([[:space:]]|$)' \
        | grep -v grep; } || echo "(none matched)"
  } >> "$LOG" 2>&1
done
