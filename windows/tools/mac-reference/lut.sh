#!/usr/bin/env bash
# Runs flxlut, the Mac's LUT baker, from the Mac's tree, where it finds its packs and
# shaders. The output path may be relative to where this is called from.
#
#   tools/mac-reference/lut.sh <output.cube> [flxlut's options]
set -euo pipefail
here="$(pwd -P)"
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
source "$root/tools/mac-reference/mac-tree.sh"
"$root/tools/mac-reference/build.sh" >&2
[[ $# -ge 1 ]] || { echo "usage: $0 <output.cube> [options]" >&2; exit 1; }
out="$1"; shift
[[ "$out" = /* ]] || out="$here/$out"
cd "$mac"
exec "$root/build/mac-reference/flxlut" "$out" "$@"
