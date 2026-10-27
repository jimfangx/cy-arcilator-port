#!/usr/bin/env bash
# Run after sourcing Chipyard env.sh. All generated files stay outside Git.
set -euo pipefail
arc_toolchain_sim_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$arc_toolchain_sim_dir/compiler/toolchain.py" build "$@"
