#!/bin/sh
# two prefix-related entries: short request, then long request (which subsumes the
# short one), then a restart and the short request again
set -u
cd "$(dirname "$0")" || exit 1

kill_18090() {
    pid=$(netstat -ano | grep -E "TCP[[:space:]]+127\.0\.0\.1:18090[[:space:]]" | grep -i listening | awk '{print $NF}' | head -1)
    if [ -n "${pid:-}" ]; then
        taskkill //F //PID "$pid" >/dev/null 2>&1
        sleep 3
    fi
}

wait_ready() {
    for i in $(seq 1 120); do
        s=$(curl -s -m 2 http://127.0.0.1:18090/health 2>/dev/null)
        if [ "$s" = '{"status":"ok"}' ]; then return 0; fi
        sleep 2
    done
    return 1
}

kill_18090
rm -rf case-two && mkdir -p case-two

echo "=== run 1: cold short request, then the long request that subsumes it ==="
cmd //c "run_case.bat case-two > logs\\two-run1.log 2>&1" &
wait_ready
python req.py 4000 "S-cold" 4 2>&1 | tail -2
sleep 12
python req.py 12000 "L-absorbs-S" 4 2>&1 | tail -2
sleep 14
echo "--- entries on disk ---"
grep -E "stored|dropping|mirrored" logs/two-run1.log | tail -6
ls case-two/*.bin
kill_18090

echo "=== run 2: restart, ask for the short prompt again ==="
cmd //c "run_case.bat case-two > logs\\two-run2.log 2>&1" &
wait_ready
python req.py 4000 "S-after-restart" 4 2>&1 | tail -2
echo "--- disk cache events ---"
grep -E "scan_end|lookup for|entry with length|candidate|restored|read_entry|checksum|no restorable" logs/two-run2.log | tail -10
kill_18090
