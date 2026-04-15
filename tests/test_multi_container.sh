#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="$ROOT_DIR/bin/engine"

if [[ ! -x "$ENGINE" ]]; then
  echo "engine binary not found; run make engine first" >&2
  exit 1
fi

{
  echo "start alpha 32 64 -- /bin/sh -c 'for i in 1 2 3; do echo alpha-\$i; sleep 1; done'"
  echo "start beta 32 64 -- /bin/sh -c 'for i in 1 2 3; do echo beta-\$i >&2; sleep 1; done'"
  sleep 2
  echo "list"
  sleep 3
  echo "quit"
} | "$ENGINE"
