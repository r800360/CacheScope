#!/usr/bin/env bash
#
# Build CacheScope and produce a report, in one command.
#
# Requires only CMake and a C++ compiler. No vcpkg, no Ninja.
#
# Usage:
#   scripts/run_experiment.sh [--preset quick|standard|deep] [--label NAME]
#                             [--repeat N] [--out DIR] [--no-open]

set -euo pipefail

preset="standard"
label="$(hostname 2>/dev/null || echo "this machine")"
repeat=1
out="results"
open_report=1

while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset="$2"; shift 2 ;;
        --label) label="$2"; shift 2 ;;
        --repeat) repeat="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --no-open) open_report=0; shift ;;
        -h|--help)
            sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake was not found on PATH. Install CMake 3.22 or newer." >&2
    exit 1
fi

echo "==> Configuring"
cmake --preset headless-release

echo "==> Building"
cmake --build --preset headless-release --parallel

echo "==> Testing"
ctest --preset headless-release

exe="build/headless-release/bin/cachescope_cli"
if [ ! -x "$exe" ]; then
    echo "built binary not found at $exe" >&2
    exit 1
fi

echo "==> Measuring ($preset preset, $repeat run(s))"
echo "    Close heavy applications and leave the machine idle for the best results."

args=(--preset "$preset" --label "$label" --repeat "$repeat" --out "$out")
[ "$open_report" -eq 1 ] && args+=(--open)
"$exe" "${args[@]}"

echo
echo "Reports are in $(cd "$out" && pwd)"
echo "Share the .csv files to compare machines:"
echo "  build/headless-release/bin/cachescope_compare --out comparison.html $out other-machine.csv"
