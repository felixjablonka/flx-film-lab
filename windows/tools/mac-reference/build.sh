#!/usr/bin/env bash
# Builds flxref (main.swift) with the Mac app's engine from its tree (mac/, or
# the root of the public repository: mac-tree.sh), the release this port
# follows: the same source files the app's own tools compile (ENGINE_SOURCES
# in its build.sh), so the reference is the app's engine.
# And flxdecode (decode.swift), Apple's RAW decoder as RawLoader sets it up,
# flxmeasure (tools/calibrate/measure.swift), which measures its colour,
# flxbaseline (tools/calibrate/baseline.swift), its baseline exposure per file, and
# flxlut, the Mac's own LUT baker (tools/lut there), for the port's to match.
#
#   tools/mac-reference/build.sh          -> build/mac-reference/flxref, flxdecode, flxmeasure, flxbaseline, flxlut
#   tools/mac-reference/run.sh ...        -> builds if needed, runs it from the Mac's tree

set -euo pipefail
cd "$(dirname "$0")/../.."
root="$(pwd -P)"
source tools/mac-reference/mac-tree.sh

out="build/mac-reference"
mkdir -p "$out"
sources=()
while IFS= read -r line; do sources+=("$mac/$line"); done < <(
  sed -n '/^ENGINE_SOURCES=(/,/^)/p' "$mac/build.sh" | sed -n 's/^ *\(FLXFilmLab\/[^ ]*\.swift\).*/\1/p')
[[ ${#sources[@]} -gt 10 ]] || { echo "error: no ENGINE_SOURCES in $mac/build.sh" >&2; exit 1; }
target="$(sed -n 's/^DEPLOYMENT_TARGET="\([0-9.]*\)".*/\1/p' "$mac/build.sh")"

if [[ ! -x "$out/flxref" || -n "$(find "${sources[@]}" tools/mac-reference/main.swift "$mac/build.sh" -newer "$out/flxref")" ]]; then
  echo "==> building flxref against $mac ($(git -C "$mac" describe --tags --always))"
  swiftc -O -target "arm64-apple-macosx${target:-26.0}" -module-name FLXRef \
    -o "$out/flxref" "${sources[@]}" tools/mac-reference/main.swift
fi
# flxdecode needs none of the engine: RawLoader's settings are repeated in it.
if [[ ! -x "$out/flxdecode" || tools/mac-reference/decode.swift -nt "$out/flxdecode" ]]; then
  echo "==> building flxdecode"
  swiftc -O -target "arm64-apple-macosx${target:-26.0}" -o "$out/flxdecode" tools/mac-reference/decode.swift
fi
if [[ ! -x "$out/flxmeasure" || tools/calibrate/measure.swift -nt "$out/flxmeasure" ]]; then
  echo "==> building flxmeasure"
  swiftc -O -target "arm64-apple-macosx${target:-26.0}" -o "$out/flxmeasure" tools/calibrate/measure.swift
fi
# flxbaseline: Apple's baseline exposure per file, for tools/calibrate/iso_exposure.py
if [[ ! -x "$out/flxbaseline" || tools/calibrate/baseline.swift -nt "$out/flxbaseline" ]]; then
  echo "==> building flxbaseline"
  swiftc -O -target "arm64-apple-macosx${target:-26.0}" -o "$out/flxbaseline" tools/calibrate/baseline.swift
fi
# flxlut runs from the Mac's tree, where its packs and shaders are
# (tools/mac-reference/lut.sh). It imports the engine as the module FLXCore, as
# the Mac's build.sh compiles it.
# The lists nest (CORE_SOURCES names MOTION_CORE_SOURCES, which names
# ENGINE_SOURCES), so bash itself reads them, in a shell of their own.
core_sources=()
while IFS= read -r line; do core_sources+=("$mac/$line"); done < <(
  bash -c "$(sed -n '/^ENGINE_SOURCES=(/,/^)/p; /^MOTION_CORE_SOURCES=(/,/^)/p; /^CORE_SOURCES=(/,/^)/p' "$mac/build.sh")
           printf '%s\n' \"\${CORE_SOURCES[@]}\"")
if [[ ! -x "$out/flxlut" || -n "$(find "${core_sources[@]}" "$mac/tools/lut/main.swift" -newer "$out/flxlut")" ]]; then
  echo "==> building flxlut against $mac ($(git -C "$mac" describe --tags --always))"
  mkdir -p "$out/core"
  swiftc -O -enable-testing -target "arm64-apple-macosx${target:-26.0}" -module-name FLXCore \
    -emit-module -emit-module-path "$out/core/FLXCore.swiftmodule" \
    -emit-library -static -o "$out/core/libFLXCore.a" "${core_sources[@]}"
  swiftc -O -target "arm64-apple-macosx${target:-26.0}" -I "$out/core" \
    -Xlinker -force_load -Xlinker "$out/core/libFLXCore.a" -o "$out/flxlut" "$mac/tools/lut/main.swift"
fi
# flxmotion: the Mac's Motion pipeline on one frame, against FLXCore too
if [[ ! -x "$out/flxmotion" || tools/mac-reference/motionframe.swift -nt "$out/flxmotion" || "$out/core/libFLXCore.a" -nt "$out/flxmotion" ]]; then
  echo "==> building flxmotion"
  swiftc -O -target "arm64-apple-macosx${target:-26.0}" -I "$out/core" \
    -Xlinker -force_load -Xlinker "$out/core/libFLXCore.a" -o "$out/flxmotion" tools/mac-reference/motionframe.swift
fi
