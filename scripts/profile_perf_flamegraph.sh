#!/bin/sh

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COMPOSE_FILES="-f $ROOT_DIR/docker-compose.yml -f $ROOT_DIR/docker-compose.perf.yml"
REPORT_ROOT="${REPORT_ROOT:-$ROOT_DIR/reports/perf}"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
REPORT_DIR="${REPORT_DIR:-$REPORT_ROOT/$TIMESTAMP}"
BASE_URL="${BASE_URL:-http://127.0.0.1:9006}"
TARGET_URL="${TARGET_URL:-$BASE_URL/healthz}"
CONNECTIONS="${CONNECTIONS:-200}"
THREADS="${THREADS:-4}"
DURATION="${DURATION:-15s}"
SAMPLE_FREQ="${SAMPLE_FREQ:-99}"
LUA_SCRIPT="${LUA_SCRIPT:-}"
STACK_MODE="${STACK_MODE:-dwarf}"
PERF_DATA_IN_CONTAINER="/app/reports/perf/$TIMESTAMP/perf.data"
PERF_SCRIPT_IN_CONTAINER="/app/reports/perf/$TIMESTAMP/perf.unfolded"
FOLDED_IN_CONTAINER="/app/reports/perf/$TIMESTAMP/perf.folded"
SVG_IN_CONTAINER="/app/reports/perf/$TIMESTAMP/flamegraph.svg"
WRK_OUT="$REPORT_DIR/wrk.txt"
PERF_RECORD_OUT="$REPORT_DIR/perf-record.txt"
PERF_BIN_IN_CONTAINER="${PERF_BIN_IN_CONTAINER:-}"

mkdir -p "$REPORT_DIR"

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 1
    fi
}

wait_for_healthz() {
    i=0
    while [ "$i" -lt 60 ]; do
        if curl -fsS "$BASE_URL/healthz" >/dev/null 2>&1; then
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done

    echo "web service did not become healthy: $BASE_URL/healthz" >&2
    exit 1
}

run_wrk() {
    if [ -n "$LUA_SCRIPT" ]; then
        wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" --latency -s "$LUA_SCRIPT" "$TARGET_URL" > "$WRK_OUT"
    else
        wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" --latency "$TARGET_URL" > "$WRK_OUT"
    fi
}

require_cmd docker
require_cmd curl
require_cmd wrk

echo "==> starting profiling stack"
docker compose $COMPOSE_FILES up -d --build

echo "==> waiting for service"
wait_for_healthz

SERVER_PID="$(docker compose $COMPOSE_FILES exec -T web sh -lc 'pidof server' | tr -d '\r' | awk '{print $1}')"
if [ -z "$SERVER_PID" ]; then
    echo "failed to locate server pid in container" >&2
    exit 1
fi

if [ -z "$PERF_BIN_IN_CONTAINER" ]; then
    PERF_BIN_IN_CONTAINER="$(docker compose $COMPOSE_FILES exec -T web sh -lc 'for candidate in /usr/lib/linux-tools/*/perf; do if [ -x "$candidate" ]; then printf "%s\n" "$candidate"; exit 0; fi; done; command -v perf 2>/dev/null || true' | tr -d '\r')"
fi

if [ -z "$PERF_BIN_IN_CONTAINER" ]; then
    echo "failed to locate perf binary in container" >&2
    exit 1
fi

echo "==> profiling pid=$SERVER_PID"
echo "report_dir=$REPORT_DIR"
echo "target_url=$TARGET_URL"
echo "connections=$CONNECTIONS"
echo "threads=$THREADS"
echo "duration=$DURATION"
echo "sample_freq=$SAMPLE_FREQ"
echo "stack_mode=$STACK_MODE"
echo "perf_bin=$PERF_BIN_IN_CONTAINER"

docker compose $COMPOSE_FILES exec -T web sh -lc \
    "mkdir -p /app/reports/perf/$TIMESTAMP && $PERF_BIN_IN_CONTAINER record -F $SAMPLE_FREQ -g --call-graph $STACK_MODE -p $SERVER_PID -o $PERF_DATA_IN_CONTAINER -- sleep $DURATION" \
    > "$PERF_RECORD_OUT" 2>&1 &
PERF_PID=$!

sleep 1
run_wrk

wait "$PERF_PID"

echo "==> generating flamegraph"
docker compose $COMPOSE_FILES exec -T web sh -lc \
    "$PERF_BIN_IN_CONTAINER script -i $PERF_DATA_IN_CONTAINER > $PERF_SCRIPT_IN_CONTAINER && /opt/FlameGraph/stackcollapse-perf.pl $PERF_SCRIPT_IN_CONTAINER > $FOLDED_IN_CONTAINER && /opt/FlameGraph/flamegraph.pl --title 'Atlas WebServer perf flamegraph' $FOLDED_IN_CONTAINER > $SVG_IN_CONTAINER"

echo "==> artifacts"
echo "$WRK_OUT"
echo "$REPORT_DIR/perf-record.txt"
echo "$REPORT_DIR/perf.data"
echo "$REPORT_DIR/perf.unfolded"
echo "$REPORT_DIR/perf.folded"
echo "$REPORT_DIR/flamegraph.svg"
