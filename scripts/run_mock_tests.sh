#!/usr/bin/env bash
# Run the sqllogictests, including the end-to-end ones, against the local mock TypeSafe server.
# Usage: scripts/run_mock_tests.sh [release|debug]
set -euo pipefail

BUILD="${1:-release}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UNITTEST="$ROOT/build/$BUILD/test/unittest"

LOG="$(mktemp)"
python3 "$ROOT/test/mock/mock_typesafe_server.py" 0 >"$LOG" 2>&1 &
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null || true; rm -f "$LOG"' EXIT

for _ in $(seq 50); do
  grep -q READY "$LOG" && break
  sleep 0.1
done
PORT="$(awk '/READY/ {print $2}' "$LOG")"
if [ -z "$PORT" ]; then
  echo "mock server failed to start:" >&2
  cat "$LOG" >&2
  exit 1
fi

export JEV_MOCK_URL="http://127.0.0.1:$PORT"
cd "$ROOT"
"$UNITTEST" "test/*"
