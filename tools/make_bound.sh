#!/usr/bin/env bash
set -euo pipefail
if (( $# < 2 )); then
  echo 'Usage: make_bound.sh FIGHTER MOVE [--apply]' >&2
  exit 2
fi
fighter=$1
move=$2
shift 2
exec python3 "$(dirname "$0")/move_edit.py" bound "$fighter" "$move" on "$@"
