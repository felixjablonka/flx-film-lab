#!/usr/bin/env bash
# Runs flxref from the Mac's tree, where the engine finds its packs and shaders.
# Paths may be relative to where this is called from.
#
#   tools/mac-reference/run.sh <raw> <outdir> [--film STOCK] [--paper STOCK] [--size PX] [--plain]

set -euo pipefail
here="$(pwd -P)"
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
source "$root/tools/mac-reference/mac-tree.sh"
"$root/tools/mac-reference/build.sh"
[[ $# -ge 2 ]] || { echo "usage: $0 <raw> <outdir> [options]" >&2; exit 1; }
abs() { [[ "$1" = /* ]] && echo "$1" || echo "$here/$1"; }
raw="$(abs "$1")"; outdir="$(abs "$2")"; shift 2
cd "$mac"
FLX_MAC_RELEASE="$(git describe --tags --always)" exec "$root/build/mac-reference/flxref" "$raw" "$outdir" "$@"
