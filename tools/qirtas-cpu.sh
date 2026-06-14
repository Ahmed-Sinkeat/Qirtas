#!/usr/bin/env bash
# Sample Qirtas CPU% over time, one line per interval.
#
# Usage:  tools/qirtas-cpu.sh [interval_seconds]   (default 0.5)
# Stop:   Ctrl-C
#
# Pair with the app's own perf log:
#   QIRTAS_PERF=2 ./zig-out/bin/qirtas <workspace> 2>perf.log &
#   tools/qirtas-cpu.sh 0.5 | tee cpu.log
# Then do ONE action at a time (type / scroll / paste) and note the time.
set -u

INT=${1:-0.5}
CLK=$(getconf CLK_TCK 2>/dev/null || echo 100)

pid=$(pgrep -f 'bin/qirtas' | head -1)
if [ -z "$pid" ]; then
    echo "qirtas not running (pgrep -f bin/qirtas found nothing)" >&2
    exit 1
fi

echo "# sampling pid=$pid interval=${INT}s CLK_TCK=$CLK  (Ctrl-C to stop)"
echo "# time           CPU%"

prev=0
first=1
while kill -0 "$pid" 2>/dev/null; do
    stat=$(cat "/proc/$pid/stat" 2>/dev/null) || break
    # Skip "pid (comm) " — comm may contain spaces/parens — then count fields
    # from #3 (state). utime=field14 -> $12, stime=field15 -> $13.
    rest=${stat#*") "}
    # shellcheck disable=SC2086
    set -- $rest
    cur=$(( ${12} + ${13} ))
    if [ "$first" -eq 0 ]; then
        d=$(( cur - prev ))
        cpu=$(awk "BEGIN{printf \"%.1f\", ($d/$CLK)/$INT*100}")
        printf '%s  %6s\n' "$(date +%H:%M:%S.%2N)" "$cpu"
    fi
    prev=$cur
    first=0
    sleep "$INT"
done
echo "# qirtas exited"
