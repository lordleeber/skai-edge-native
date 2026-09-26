#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SKAI_SMOKE_PYTHON:-python3}" -B "$script_dir/jetson_smoke.py" "$@"
