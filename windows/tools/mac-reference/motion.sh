#!/usr/bin/env bash
# Runs flxmotion, the Mac's Motion pipeline on one frame, from the Mac's tree, where it
# finds its packs and shaders. Paths may be relative to where this is called.
#
#   tools/mac-reference/motion.sh <movie> <out.f32> [flxmotion's options]
set -euo pipefail
here="$(pwd -P)"
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
source "$root/tools/mac-reference/mac-tree.sh"
"$root/tools/mac-reference/build.sh" >&2
[[ $# -ge 2 ]] || { echo "usage: $0 <movie> <out.f32> [options]" >&2; exit 1; }
abs() { [[ "$1" = /* ]] && echo "$1" || echo "$here/$1"; }
movie="$(abs "$1")"; out="$(abs "$2")"; shift 2
cd "$mac"
exec "$root/build/mac-reference/flxmotion" "$movie" "$out" "$@"
