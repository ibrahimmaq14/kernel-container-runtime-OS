#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="$ROOT_DIR/bin/engine"
LOG_DIR="$ROOT_DIR/logs"

rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

{
  echo "start logger 32 64 -- /bin/sh -c 'echo out-line; echo err-line >&2; sleep 1'"
  sleep 2
  echo "quit"
} | "$ENGINE"

log_file="$(find "$LOG_DIR" -name '*_logger.log' | head -n 1)"

if [[ -z "$log_file" ]]; then
  echo "log file not created" >&2
  exit 1
fi

grep -q "out-line" "$log_file"
grep -q "err-line" "$log_file"
echo "logging test passed: $log_file"
