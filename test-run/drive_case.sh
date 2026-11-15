#!/bin/sh
# run one tamper case end to end: start server, wait, one request, dump the disk-cache lines
set -u
case_name="$1"
cd "$(dirname "$0")" || exit 1

# stop any instance still listening on the test port
pid=$(netstat -ano | grep -E "TCP[[:space:]]+127\.0\.0\.1:18090[[:space:]]" | grep -i listening | awk '{print $NF}' | head -1)
if [ -n "${pid:-}" ]; then
    taskkill //F //PID "$pid" >/dev/null 2>&1
    sleep 3
fi

cmd //c "run_case.bat $case_name > logs\\$case_name.log 2>&1" &
for i in $(seq 1 120); do
    s=$(curl -s -m 2 http://127.0.0.1:18090/health 2>/dev/null)
    if [ "$s" = '{"status":"ok"}' ]; then break; fi
    sleep 2
done

python req.py 12000 "$case_name" 4 2>&1 | tail -3
echo "--- disk cache events ---"
grep -E "checksum|dropping|no restorable|read_entry|lookup for|falling back|scan_manifest|scan_end" "logs/$case_name.log" | tail -10

pid=$(netstat -ano | grep -E "TCP[[:space:]]+127\.0\.0\.1:18090[[:space:]]" | grep -i listening | awk '{print $NF}' | head -1)
if [ -n "${pid:-}" ]; then
    taskkill //F //PID "$pid" >/dev/null 2>&1
    sleep 2
fi
