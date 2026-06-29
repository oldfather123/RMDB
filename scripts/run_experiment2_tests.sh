#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$ROOT/tests/experiment2"
RESULT_DIR="$TEST_DIR/results"
SERVER="$ROOT/build/bin/rmdb"
CLIENT="$ROOT/rmdb_client/build/rmdb_client"
LOCAL_LIB="$ROOT/.local-tools/usr/lib/x86_64-linux-gnu"

if [[ ! -x "$SERVER" ]]; then
  echo "missing server binary: $SERVER" >&2
  echo "build it with: cmake -S . -B build && cmake --build build -j\$(nproc)" >&2
  exit 1
fi

if [[ ! -x "$CLIENT" ]]; then
  echo "missing client binary: $CLIENT" >&2
  echo "build it with: cmake -S rmdb_client -B rmdb_client/build && cmake --build rmdb_client/build -j\$(nproc)" >&2
  exit 1
fi

mkdir -p "$RESULT_DIR"
rm -f "$RESULT_DIR"/*.actual "$RESULT_DIR"/*.diff "$RESULT_DIR"/*.client.log "$RESULT_DIR"/*.server.log

cleanup_server() {
  local pid="${1:-}"
  if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    for _ in {1..20}; do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
  fi
}

run_case() {
  local sql_file="$1"
  local name
  name="$(basename "$sql_file" .sql)"
  local expected="$TEST_DIR/$name.expected"
  local actual="$RESULT_DIR/$name.actual"
  local server_log="$RESULT_DIR/$name.server.log"
  local client_log="$RESULT_DIR/$name.client.log"
  local diff_file="$RESULT_DIR/$name.diff"
  local db_name="exp2_${name}_db"

  if [[ ! -f "$expected" ]]; then
    echo "missing expected file: $expected" >&2
    return 1
  fi

  rm -rf "$ROOT/build/$db_name"
  (
    cd "$ROOT/build"
    exec "$SERVER" "$db_name" > "$server_log" 2>&1
  ) &
  local server_pid=$!

  sleep 0.5
  if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "[FAIL] $name: server exited early" >&2
    cat "$server_log" >&2 || true
    return 1
  fi

  set +e
  {
    cat "$sql_file"
    printf '\nexit\n'
  } | LD_LIBRARY_PATH="$LOCAL_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$CLIENT" > "$client_log" 2>&1
  local client_rc=$?
  set -e

  cleanup_server "$server_pid"

  if [[ $client_rc -ne 0 ]]; then
    echo "[FAIL] $name: client exited with $client_rc" >&2
    cat "$client_log" >&2 || true
    return 1
  fi

  if [[ -f "$ROOT/build/$db_name/output.txt" ]]; then
    cp "$ROOT/build/$db_name/output.txt" "$actual"
  else
    : > "$actual"
  fi

  if diff -u "$expected" "$actual" > "$diff_file"; then
    echo "[PASS] $name"
    rm -f "$diff_file"
    rm -rf "$ROOT/build/$db_name"
    return 0
  fi

  echo "[FAIL] $name"
  echo "  expected: $expected"
  echo "  actual:   $actual"
  echo "  diff:     $diff_file"
  return 1
}

failures=0
for sql_file in "$TEST_DIR"/*.sql; do
  if ! run_case "$sql_file"; then
    failures=$((failures + 1))
  fi
done

if [[ $failures -ne 0 ]]; then
  echo "$failures experiment 2 test(s) failed" >&2
  exit 1
fi

echo "all experiment 2 tests passed"
