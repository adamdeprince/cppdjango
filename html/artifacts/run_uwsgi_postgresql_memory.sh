#!/usr/bin/env bash
# Measure stock/native uWSGI memory and post-fork COW behavior.
set -euo pipefail

ROOT="${ROOT:-$HOME/cppdjango-bench}"
APP="$ROOT/app/hello"
RESULTS="$ROOT/results"
LOGS="$ROOT/logs/postgres-orm-memory"
UWSGI_BIN="${UWSGI_BIN:-$HOME/DCPerf/venvs/django_workload_native/bin/uwsgi}"
WRK_BIN="${WRK_BIN:-/usr/bin/wrk}"
SIEGE_BIN="${SIEGE_BIN:-/usr/bin/siege}"
PROFILE_BIN="${PROFILE_BIN:-/usr/bin/python3}"
PROFILE_SCRIPT="${PROFILE_SCRIPT:-$ROOT/profile_uwsgi_memory.py}"
MIX_SCRIPT="${MIX_SCRIPT:-$ROOT/uwsgi_memory_mix.lua}"
SIEGE_RC="${SIEGE_RC:-$ROOT/siege_memory.conf}"
PORT="${PORT:-8081}"
HOST="127.0.0.1"
WORKERS="${WORKERS:-16}"
CONCURRENCY="${CONCURRENCY:-64}"
THREADS="${THREADS:-8}"
WARMUP="${WARMUP:-6}"
LOAD_DURATION="${LOAD_DURATION:-24}"
LOAD_KIND="${LOAD_KIND:-duration}"
REQUESTS_PER_ENDPOINT="${REQUESTS_PER_ENDPOINT:-100000}"
WARMUP_REQUESTS_PER_ENDPOINT="${WARMUP_REQUESTS_PER_ENDPOINT:-5000}"
REPEATS="${REPEATS:-3}"
FORK_MODES="${FORK_MODES:-lazy preload}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RAW="$RESULTS/postgres_orm_memory_${STAMP}.jsonl"
LOAD_CSV="$RESULTS/postgres_orm_memory_${STAMP}_LOAD.csv"
META="$RESULTS/postgres_orm_memory_${STAMP}_META.txt"
NATIVE_PATCH="$RESULTS/postgres_orm_memory_${STAMP}_NATIVE.patch"
PIDFILE="$ROOT/postgres-orm-memory.pid"
SERVER_PID=""
LOAD_PID=""

mkdir -p "$RESULTS" "$LOGS"
export DJANGO_SETTINGS_MODULE=hello.settings_local
export DJANGO_DB=postgresql
export DB_NAME=hello_world
export DB_USER=benchmarkdbuser
export DB_PASSWORD=benchmarkdbpass
export DB_HOST=127.0.0.1
export DB_PORT=""
export BENCH_MW=orm_isolation
export PYTHONHASHSEED=0

stop_load() {
  if [ -n "$LOAD_PID" ] && kill -0 "$LOAD_PID" 2>/dev/null; then
    kill -TERM "$LOAD_PID" 2>/dev/null || true
  fi
  if [ -n "$LOAD_PID" ]; then
    wait "$LOAD_PID" 2>/dev/null || true
  fi
  LOAD_PID=""
}

stop_server() {
  local pid=""
  stop_load
  if [ -f "$PIDFILE" ]; then
    pid="$(sed -n '1p' "$PIDFILE" 2>/dev/null || true)"
  elif [ -n "$SERVER_PID" ]; then
    pid="$SERVER_PID"
  fi
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid" 2>/dev/null || true
    for _ in $(seq 1 150); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL "$pid" 2>/dev/null || true
    fi
  fi
  if [ -n "$SERVER_PID" ]; then
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  SERVER_PID=""
  rm -f "$PIDFILE"
}
trap stop_server EXIT INT TERM

variant_env() {
  local variant="$1"
  local venv="$ROOT/venvs/$variant"
  export VIRTUAL_ENV="$venv"
  export PATH="$venv/bin:/usr/local/bin:/usr/bin:/bin"
  if [ "$variant" = "native" ]; then
    export DJANGO_NATIVE=1
    export PYTHONPATH="$ROOT/src/cppdjango:$APP"
  else
    export DJANGO_NATIVE=0
    export PYTHONPATH="$APP"
  fi
}

child_count() {
  local master_pid="$1"
  local children_file="/proc/$master_pid/task/$master_pid/children"
  if [ ! -r "$children_file" ]; then
    echo 0
    return
  fi
  wc -w < "$children_file" | tr -d ' '
}

snapshot() {
  local repeat="$1" variant="$2" fork_mode="$3" stage="$4"
  local master_pid
  master_pid="$(sed -n '1p' "$PIDFILE")"
  "$PROFILE_BIN" "$PROFILE_SCRIPT" \
    --master-pid "$master_pid" \
    --expected-workers "$WORKERS" \
    --variant "$variant" \
    --fork-mode "$fork_mode" \
    --stage "$stage" \
    --repeat "$repeat" >> "$RAW"
  echo "SNAPSHOT repeat=$repeat variant=$variant fork_mode=$fork_mode stage=$stage"
}

start_server() {
  local variant="$1" fork_mode="$2" server_log="$3"
  local ready_required=1
  local args=(
    --http11-socket "$HOST:$PORT"
    --chdir "$APP"
    --module hello.wsgi:application
    --env DJANGO_SETTINGS_MODULE=hello.settings_local
    --wsgi-env-behavior holy
    --master
    --py-call-uwsgi-fork-hooks
    --thunder-lock
    --need-app
    --die-on-term
    --vacuum
    --harakiri 75
    --reload-mercy 15
    --worker-reload-mercy 15
    --listen 1024
    --buffer-size 8192
    --socket-timeout 30
    --http-timeout 30
    --http-keepalive
    --http-auto-chunked
    --disable-logging
    --home "$ROOT/venvs/$variant"
    --workers "$WORKERS"
    --pidfile "$PIDFILE"
  )

  variant_env "$variant"
  stop_server
  if ss -ltnH "sport = :$PORT" | grep -q .; then
    echo "port $PORT is already in use" >&2
    return 1
  fi
  if [ "$fork_mode" = "lazy" ]; then
    args+=(--lazy-apps)
    ready_required="$WORKERS"
  fi
  : > "$server_log"
  "$UWSGI_BIN" "${args[@]}" >"$server_log" 2>&1 &
  SERVER_PID=$!

  for _ in $(seq 1 300); do
    if [ -s "$PIDFILE" ]; then
      local master_pid ready children
      master_pid="$(sed -n '1p' "$PIDFILE")"
      ready="$(grep -c 'WSGI app 0 .* ready' "$server_log" || true)"
      children="$(child_count "$master_pid")"
      if [ "$ready" -ge "$ready_required" ] && [ "$children" -eq "$WORKERS" ]; then
        sleep 1
        return
      fi
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      tail -100 "$server_log" >&2
      return 1
    fi
    sleep 0.2
  done
  tail -100 "$server_log" >&2
  echo "uWSGI did not reach the expected process state" >&2
  return 1
}

verify_identity() {
  local variant="$1"
  local identity
  identity="$(curl -fsS --max-time 5 "http://$HOST:$PORT/bench-info")"
  if [ "$variant" = "native" ]; then
    grep -q '"native_available":true' <<<"$identity"
    grep -q '/src/cppdjango/django/__init__.py' <<<"$identity"
  else
    grep -q '"native_available":false' <<<"$identity"
    grep -q '/venvs/stock/' <<<"$identity"
  fi
}

run_duration_load() {
  local repeat="$1" variant="$2" fork_mode="$3" server_log="$4"
  local raw_file="$LOGS/${STAMP}_r${repeat}_${fork_mode}_${variant}.wrk"
  local sample_gap=$((LOAD_DURATION / 4))
  if [ "$sample_gap" -lt 1 ]; then sample_gap=1; fi

  "$WRK_BIN" -t"$THREADS" -c"$CONCURRENCY" -d"${WARMUP}s" \
    -s "$MIX_SCRIPT" "http://$HOST:$PORT" >/dev/null
  snapshot "$repeat" "$variant" "$fork_mode" warmed

  "$WRK_BIN" -t"$THREADS" -c"$CONCURRENCY" -d"${LOAD_DURATION}s" \
    --latency -s "$MIX_SCRIPT" "http://$HOST:$PORT" >"$raw_file" 2>&1 &
  LOAD_PID=$!
  for sample in 1 2 3; do
    sleep "$sample_gap"
    if ! kill -0 "$LOAD_PID" 2>/dev/null; then
      echo "wrk exited before memory sample $sample" >&2
      return 1
    fi
    snapshot "$repeat" "$variant" "$fork_mode" "loaded_$sample"
  done
  wait "$LOAD_PID"
  LOAD_PID=""
  sleep 2
  snapshot "$repeat" "$variant" "$fork_mode" settled

  local requests rps p50 p99 errors
  requests="$(awk '/requests in/ {print $1; exit}' "$raw_file")"
  rps="$(awk '/Requests\/sec/ {print $2; exit}' "$raw_file")"
  p50="$(awk '$1 == "50%" {print $2; exit}' "$raw_file")"
  p99="$(awk '$1 == "99%" {print $2; exit}' "$raw_file")"
  errors=0
  if grep -Eq 'Socket errors|Non-2xx or 3xx responses' "$raw_file"; then errors=1; fi
  if [ -z "$requests" ] || [ -z "$rps" ]; then
    echo "wrk output could not be parsed: $raw_file" >&2
    return 1
  fi
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$repeat" "$variant" "$fork_mode" \
    "$LOAD_KIND" mixed "$WORKERS" "$CONCURRENCY" "$LOAD_DURATION" "$requests" "$rps" \
    "$p50" "$p99" "$errors" >> "$LOAD_CSV"

  if grep -Eq 'Fatal Python error|Traceback|died, killed by signal|no python application found' "$server_log"; then
    tail -100 "$server_log" >&2
    return 1
  fi
  echo "RESULT repeat=$repeat variant=$variant fork_mode=$fork_mode rps=$rps errors=$errors"
}

run_equal_work_load() {
  local repeat="$1" variant="$2" fork_mode="$3" server_log="$4"
  local url_file="$LOGS/${STAMP}_r${repeat}_${fork_mode}_${variant}.urls"
  local warm_file="$LOGS/${STAMP}_r${repeat}_${fork_mode}_${variant}_warm.siege.json"
  local raw_file="$LOGS/${STAMP}_r${repeat}_${fork_mode}_${variant}.siege.json"
  local target_requests=$((REQUESTS_PER_ENDPOINT * 3))
  local warm_target=$((WARMUP_REQUESTS_PER_ENDPOINT * 3))
  local repetitions=$(((target_requests + CONCURRENCY - 1) / CONCURRENCY))
  local warm_repetitions=$(((warm_target + CONCURRENCY - 1) / CONCURRENCY))
  local expected_requests=$((repetitions * CONCURRENCY))

  printf '%s\n' \
    "http://$HOST:$PORT/db" \
    "http://$HOST:$PORT/orm-in" \
    "http://$HOST:$PORT/orm-update?queries=1" > "$url_file"
  "$SIEGE_BIN" -R "$SIEGE_RC" -c "$CONCURRENCY" -r "$warm_repetitions" \
    -f "$url_file" > "$warm_file"
  snapshot "$repeat" "$variant" "$fork_mode" warmed

  "$SIEGE_BIN" -R "$SIEGE_RC" -c "$CONCURRENCY" -r "$repetitions" \
    -f "$url_file" > "$raw_file"

  local duration requests rps failed errors
  duration="$(awk -F: '/"elapsed_time"/ {gsub(/[ ,\t]/, "", $2); print $2; exit}' "$raw_file")"
  requests="$(awk -F: '/"transactions"/ {gsub(/[ ,\t]/, "", $2); print $2; exit}' "$raw_file")"
  rps="$(awk -F: '/"transaction_rate"/ {gsub(/[ ,\t]/, "", $2); print $2; exit}' "$raw_file")"
  failed="$(awk -F: '/"failed_transactions"/ {gsub(/[ ,\t]/, "", $2); print $2; exit}' "$raw_file")"
  errors="$failed"
  if [ "$requests" -ne "$expected_requests" ] || [ "$errors" -ne 0 ]; then
    tail -100 "$raw_file" >&2
    return 1
  fi
  snapshot "$repeat" "$variant" "$fork_mode" after_equal_work
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$repeat" "$variant" "$fork_mode" \
    "$LOAD_KIND" mixed "$WORKERS" "$CONCURRENCY" "$duration" "$requests" "$rps" \
    "" "" "$errors" >> "$LOAD_CSV"
  sleep 2
  snapshot "$repeat" "$variant" "$fork_mode" settled

  if grep -Eq 'Fatal Python error|Traceback|died, killed by signal|no python application found' "$server_log"; then
    tail -100 "$server_log" >&2
    return 1
  fi
  echo "RESULT repeat=$repeat variant=$variant fork_mode=$fork_mode exact_requests=$expected_requests errors=0"
}

run_load() {
  if [ "$LOAD_KIND" = "duration" ]; then
    run_duration_load "$@"
  elif [ "$LOAD_KIND" = "equal_work" ]; then
    run_equal_work_load "$@"
  else
    echo "LOAD_KIND must be duration or equal_work" >&2
    return 1
  fi
}

for required in "$WRK_BIN" "$SIEGE_BIN" "$PROFILE_SCRIPT" "$MIX_SCRIPT" "$SIEGE_RC"; do
  if [ ! -r "$required" ]; then
    echo "required benchmark input is unavailable: $required" >&2
    exit 1
  fi
done
if [ ! -x "$UWSGI_BIN" ] || [ ! -x "$PROFILE_BIN" ]; then
  echo "uWSGI or Python executable is unavailable" >&2
  exit 1
fi
if [ ! -r /proc/self/smaps_rollup ]; then
  echo "Linux smaps_rollup is unavailable" >&2
  exit 1
fi
if ! pg_isready -h "$DB_HOST" -d "$DB_NAME" -U "$DB_USER" >/dev/null; then
  echo "PostgreSQL is not ready" >&2
  exit 1
fi

git -C "$ROOT/src/cppdjango" diff --binary > "$NATIVE_PATCH"
{
  echo "timestamp=$STAMP"
  echo "kernel=$(uname -r)"
  echo "logical_cpus=$(nproc)"
  echo "memory_kib=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
  echo "transparent_hugepage=$(tr -d '\n' </sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo unavailable)"
  echo "ksm_run=$(cat /sys/kernel/mm/ksm/run 2>/dev/null || echo unavailable)"
  echo "postgres=$(psql --version)"
  echo "python=$($ROOT/venvs/stock/bin/python -c 'import sys; print(sys.version.split()[0])')"
  echo "psycopg=$($ROOT/venvs/stock/bin/python -c 'import psycopg; print(psycopg.__version__)')"
  echo "uwsgi=$($UWSGI_BIN --version)"
  echo "workers=$WORKERS concurrency=$CONCURRENCY threads=$THREADS"
  echo "load_kind=$LOAD_KIND warmup=$WARMUP load_duration=$LOAD_DURATION repeats=$REPEATS fork_modes=$FORK_MODES"
  echo "requests_per_endpoint=$REQUESTS_PER_ENDPOINT warmup_requests_per_endpoint=$WARMUP_REQUESTS_PER_ENDPOINT"
  echo "workload=equal wrk request rotation over point select, ordered IN32 select, and one ORM read+update"
  echo "native_base_commit=$(git -C "$ROOT/src/cppdjango" rev-parse HEAD)"
  echo "native_worktree_patch_sha256=$(sha256sum "$NATIVE_PATCH" | cut -d' ' -f1)"
  echo "harness_sha256=$(sha256sum "$0" | cut -d' ' -f1)"
  echo "profiler_sha256=$(sha256sum "$PROFILE_SCRIPT" | cut -d' ' -f1)"
  echo "mix_script_sha256=$(sha256sum "$MIX_SCRIPT" | cut -d' ' -f1)"
  echo "siege_rc_sha256=$(sha256sum "$SIEGE_RC" | cut -d' ' -f1)"
} > "$META"

: > "$RAW"
echo "timestamp,repeat,variant,fork_mode,load_kind,endpoint,workers,concurrency,duration_s,requests,rps,p50,p99,errors" > "$LOAD_CSV"

for repeat in $(seq 1 "$REPEATS"); do
  if [ $((repeat % 2)) -eq 1 ]; then
    variants=(stock native)
    modes=($FORK_MODES)
  else
    variants=(native stock)
    read -r -a modes <<<"$FORK_MODES"
    reversed_modes=()
    for ((index=${#modes[@]} - 1; index >= 0; index--)); do
      reversed_modes+=("${modes[index]}")
    done
    modes=("${reversed_modes[@]}")
  fi
  for fork_mode in "${modes[@]}"; do
    for variant in "${variants[@]}"; do
      server_log="$LOGS/${STAMP}_r${repeat}_${fork_mode}_${variant}.server.log"
      echo "START repeat=$repeat variant=$variant fork_mode=$fork_mode"
      start_server "$variant" "$fork_mode" "$server_log"
      snapshot "$repeat" "$variant" "$fork_mode" forked
      verify_identity "$variant"
      run_load "$repeat" "$variant" "$fork_mode" "$server_log"
      stop_server
    done
  done
done

echo "RAW=$RAW"
echo "LOAD_CSV=$LOAD_CSV"
echo "META=$META"
