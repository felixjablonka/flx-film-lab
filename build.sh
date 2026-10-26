#!/usr/bin/env bash
# Builds FLX Film Lab.app with swiftc and a hand-written bundle.
#
# There is no .xcodeproj on purpose: this machine has Command Line Tools but not
# Xcode, so `xcodebuild` and the offline Metal compiler are unavailable. Shaders
# are compiled at runtime from Resources/Shaders/flx.metal instead.
#
#   ./build.sh          build the app
#   ./build.sh parity   build and run the numeric parity harness
#   ./build.sh checks   build and run the invariant checks (paths parity can't see)
#   ./build.sh apptest  open, edit and switch images through the document model
#   ./build.sh lut      build flxlut, which bakes a look into a .cube LUT for log footage
#   ./build.sh lutchecks  build and run the LUT baker's checks
#   ./build.sh motiontest   run the Motion mode's video path end to end on synthetic log footage
#   ./build.sh motionapptest  open, grade, switch and export clips through the Motion mode's model
#   ./build.sh render   build flxrender, the headless renderer (the app's engine)
#   ./build.sh run      build the app and launch it
#   ./build.sh install  build it and copy it into /Applications
#   ./build.sh dmg      build it and package a shareable disk image

set -euo pipefail
cd "$(dirname "$0")"

APP_NAME="FLX Film Lab"
# The version a release is built as; the release workflow sets it from the tag.
VERSION="${VERSION:-1.1.2}"
# The binary inside the bundle, which refuse_if_running looks for.
EXECUTABLE="FLXFilmLab"
# The oldest macOS this is built to run on, on Apple Silicon only (since 1.1.2;
# 1.1.1 and before also ran on macOS 14 and 15, and on Intel). swiftc otherwise
# targets whatever the build machine runs, which once quietly produced a binary
# that refused to launch on anything older while the Info.plist advertised it.
DEPLOYMENT_TARGET="26.0"
# The first launch under this identifier takes over the settings saved under
# the app's previous one, studio.agx.AgXStudio (FLXFilmLabApp.takeOverSettings).
BUNDLE_ID="lab.flx.FLXFilmLab"
BUILD_DIR="build"
APP_DIR="$APP_NAME.app"   # project root, so it is easy to find and drag to /Applications

ENGINE_SOURCES=(
  FLXFilmLab/Model/Constants.swift
  FLXFilmLab/Model/ProfilePack.swift
  FLXFilmLab/Model/Recipe.swift
  FLXFilmLab/Model/FrameGeometry.swift
  FLXFilmLab/Engine/FilmMath.swift
  FLXFilmLab/Engine/ToneCurve.swift
  FLXFilmLab/Engine/PackLibrary.swift
  FLXFilmLab/Engine/Renderer.swift
  FLXFilmLab/Engine/RenderSetupBuilder.swift
  FLXFilmLab/Engine/MaskRasterizer.swift
  FLXFilmLab/Engine/RawLoader.swift
  FLXFilmLab/Engine/Exporter.swift
  FLXFilmLab/Effects/GlowLook.swift
  FLXFilmLab/Effects/GlowPipeline.swift
  FLXFilmLab/Effects/GlowKernels.swift
  FLXFilmLab/Effects/GlowSupport.swift
  FLXFilmLab/Engine/NLog.swift
  FLXFilmLab/Engine/Primaries.swift
  FLXFilmLab/Engine/AppleLog.swift
  FLXFilmLab/Engine/VLog.swift
  FLXFilmLab/Engine/FLog.swift
  FLXFilmLab/Engine/CanonLog.swift
  FLXFilmLab/Engine/SLog3.swift
  FLXFilmLab/Engine/ILog.swift
  FLXFilmLab/Engine/LookLUT.swift
)

# The video path: the Motion mode's log clip grading, on the same engine.
MOTION_CORE_SOURCES=(
  "${ENGINE_SOURCES[@]}"
  FLXFilmLab/Motion/Video/MotionLook.swift
  FLXFilmLab/Motion/Video/ClipInfo.swift
  FLXFilmLab/Motion/Video/FramePipeline.swift
  FLXFilmLab/Motion/Video/ClipExporter.swift
  FLXFilmLab/Motion/Video/LookBakeEngine.swift
  FLXFilmLab/Motion/Video/ClipMeter.swift
)

# One app, two modes: Stills (FLXFilmLab/) and Motion (FLXFilmLab/Motion/), switched in
# the toolbar.
APP_SOURCES=(
  "${MOTION_CORE_SOURCES[@]}"
  FLXFilmLab/Motion/Video/PreviewRenderer.swift
  FLXFilmLab/Motion/Video/PlaybackController.swift
  FLXFilmLab/Motion/App/ClipModel.swift
  FLXFilmLab/Motion/UI/PreviewView.swift
  FLXFilmLab/Motion/UI/TransportBar.swift
  FLXFilmLab/Motion/UI/MotionInspector.swift
  FLXFilmLab/Motion/UI/MotionContentView.swift
  FLXFilmLab/App/RenderEngine.swift
  FLXFilmLab/App/Presets.swift
  FLXFilmLab/App/DocumentModel.swift
  FLXFilmLab/App/AppMode.swift
  FLXFilmLab/App/FLXFilmLabApp.swift
  FLXFilmLab/UI/ControlVocabulary.swift
  FLXFilmLab/UI/Controls.swift
  FLXFilmLab/UI/CurvePlotView.swift
  FLXFilmLab/UI/MaskOverlay.swift
  FLXFilmLab/UI/CropOverlay.swift
  FLXFilmLab/UI/InspectorView.swift
  FLXFilmLab/UI/ContentView.swift
)

# What the tools and the test suites use of the app, compiled once into a
# module of its own (build/core) that they import, so each compiles only its
# own files. The app itself is still built whole, as one module.
CORE_SOURCES=(
  "${MOTION_CORE_SOURCES[@]}"
  FLXFilmLab/Motion/Video/PreviewRenderer.swift
  FLXFilmLab/Motion/Video/PlaybackController.swift
  FLXFilmLab/Motion/App/ClipModel.swift
  FLXFilmLab/App/RenderEngine.swift
  FLXFilmLab/App/Presets.swift
  FLXFilmLab/App/DocumentModel.swift
  FLXFilmLab/App/AppMode.swift
  FLXFilmLab/UI/ControlVocabulary.swift
)
CORE_DIR="$BUILD_DIR/core"

# Replacing the bundle of an app that is running breaks it: macOS can no
# longer match the running copy's code signature to the files on disk, and
# the Open and Save panels (which run out of process and check it) fail to
# appear. So a bundle is only replaced once its app has quit.
refuse_if_running() {
  local exe="$1/Contents/MacOS/$2"
  [[ "$exe" = /* ]] || exe="$(pwd)/$exe"
  if pgrep -f "$exe" >/dev/null; then
    echo "error: $1 is running. Quit it first: replacing a running app's bundle" >&2
    echo "       stops its Open and Save panels from appearing." >&2
    exit 1
  fi
}

# SwiftUI in the macOS 27 SDK implements @State as a macro, and the Command
# Line Tools do not ship the plugin that expands it — only Xcode does. So
# probe the default SDK with a one-line view and fall back to the newest
# installed SDK that still compiles it. The deployment target, not the SDK,
# decides which macOS versions the app runs on.
SDK=""
resolve_sdk() {
  [[ -n "$SDK" ]] && return 0
  mkdir -p "$BUILD_DIR"
  local probe="$BUILD_DIR/sdk-probe.swift"
  printf 'import SwiftUI\nstruct Probe: View { @State var n = 0; var body: some View { Text("\\(n)") } }\n' > "$probe"
  local default_sdk candidates=()
  default_sdk="$(xcrun --show-sdk-path)"
  candidates+=("$default_sdk")
  while IFS= read -r sdk; do candidates+=("$sdk"); done \
    < <(ls -d "$(dirname "$default_sdk")"/MacOSX[0-9]*.sdk 2>/dev/null | sort -V -r)
  for sdk in "${candidates[@]}"; do
    if swiftc -typecheck -parse-as-library -sdk "$sdk" \
         -target "arm64-apple-macosx$DEPLOYMENT_TARGET" "$probe" >/dev/null 2>&1; then
      SDK="$sdk"
      break
    fi
  done
  rm -f "$probe"
  if [[ -z "$SDK" ]]; then
    echo "error: no installed macOS SDK can compile SwiftUI with this toolchain" >&2
    exit 1
  fi
  [[ "$SDK" != "$default_sdk" ]] && echo "    using $(basename "$SDK") (the default SDK needs Xcode's SwiftUI macro plugin)"
  # The linker records the SDK a binary was built with, and macOS runs SwiftUI
  # and AppKit as that SDK's version behaves. Given only -sdk, it recorded the
  # default SDK (27.0) here while compiling against 26.5, so this Mac's builds
  # ran as 27 apps and GitHub's as 26 ones: 1.1.1's empty format field showed
  # only in the release. Naming the SDK here makes both record the same.
  export SDKROOT="$SDK"
  return 0
}

# The shared module, rebuilt only when one of its sources or this script is
# newer than it, or the SDK has changed. It is built for testing, so the tools
# and suites `@testable import FLXCore` and reach its internal API, as they did
# when each compiled the sources into itself.
build_core() {
  resolve_sdk
  local lib="$CORE_DIR/libFLXCore.a"
  if [[ -f "$lib" && "$(cat "$CORE_DIR/sdk" 2>/dev/null)" == "$SDK" \
        && -z "$(find "${CORE_SOURCES[@]}" build.sh -newer "$lib")" ]]; then
    return 0
  fi
  echo "==> building the shared engine module"
  mkdir -p "$CORE_DIR"
  swiftc -O -enable-testing -sdk "$SDK" -module-name FLXCore \
    -emit-module -emit-module-path "$CORE_DIR/FLXCore.swiftmodule" \
    -emit-library -static -o "$lib" "${CORE_SOURCES[@]}"
  echo "$SDK" > "$CORE_DIR/sdk"
}

# A tool or suite, compiled against the shared module. -force_load links all
# of it, not only the objects the linker sees referenced.
swiftc_core() {
  swiftc -O -sdk "$SDK" -I "$CORE_DIR" -Xlinker -force_load -Xlinker "$CORE_DIR/libFLXCore.a" "$@"
}

build_parity() {
  build_core
  echo "==> building parity harness"
  swiftc_core -o "$BUILD_DIR/parity" tests/parity/main.swift
  echo "==> running parity harness"
  "./$BUILD_DIR/parity"
}

build_apptest() {
  build_core
  echo "==> building app behaviour test"
  swiftc_core -o "$BUILD_DIR/apptest" tests/app/main.swift
  echo "==> running app behaviour test"
  "./$BUILD_DIR/apptest"
}

build_checks() {
  build_core
  echo "==> building invariant checks"
  swiftc_core -o "$BUILD_DIR/checks" tests/checks/main.swift
  echo "==> running invariant checks"
  "./$BUILD_DIR/checks"
}

build_lut() {
  build_core
  echo "==> building flxlut"
  swiftc_core -o "$BUILD_DIR/flxlut" tools/lut/main.swift
  echo "==> $(pwd)/$BUILD_DIR/flxlut"
}

build_lutchecks() {
  build_core
  echo "==> building LUT checks"
  swiftc_core -o "$BUILD_DIR/lutchecks" tests/common/Colour.swift tests/lut/main.swift
  echo "==> running LUT checks"
  "./$BUILD_DIR/lutchecks"
}

build_render() {
  build_core
  echo "==> building flxrender"
  swiftc_core -o "$BUILD_DIR/flxrender" tools/render/main.swift
  echo "==> $(pwd)/$BUILD_DIR/flxrender"
}

build_motiontest() {
  build_core
  echo "==> building video checks"
  swiftc_core -o "$BUILD_DIR/motiontest" tests/common/Colour.swift tests/motion/TestClip.swift tests/motion/main.swift
  echo "==> running video checks"
  "./$BUILD_DIR/motiontest"
}

# Every source typechecked as GitHub's compiler would: this Mac's newer Swift
# with its constraint solver's newer shortcuts turned off. Xcode 26's compiler
# on the macos-26 runners gave up on expressions the newer one took in its
# stride; with these flags the newer one gives up on the same ones. Run it
# before publishing. It needs a compiler that knows the flags (Swift 6.4).
build_strict() {
  build_core
  local strict=(-Xfrontend -solver-disable-prune-disjunctions -Xfrontend -solver-disable-prepared-overloads
                -Xfrontend -solver-disable-optimize-operator-defaults -Xfrontend -solver-disable-performance-hacks)
  echo "==> typechecking as GitHub's compiler would"
  swiftc -typecheck -parse-as-library -sdk "$SDK" -target "arm64-apple-macosx$DEPLOYMENT_TARGET" "${strict[@]}" \
    "${APP_SOURCES[@]}" 2>&1 | { grep -E "error:" || true; }
  local sources
  for sources in "tests/checks/main.swift" "tests/app/main.swift" "tests/parity/main.swift" \
                 "tests/common/Colour.swift tests/lut/main.swift" \
                 "tests/common/Colour.swift tests/motion/TestClip.swift tests/motion/main.swift" \
                 "tests/motion/TestClip.swift tests/motionapp/main.swift" \
                 "tools/render/main.swift" "tools/lut/main.swift"; do
    # shellcheck disable=SC2086
    swiftc -typecheck -sdk "$SDK" -I "$CORE_DIR" "${strict[@]}" $sources 2>&1 | { grep -E "error:" || true; }
  done
}

build_app() {
  refuse_if_running "$APP_DIR" "$EXECUTABLE"
  echo "==> building $APP_NAME.app"
  resolve_sdk

  # Apple Silicon only. Compiled before touching the existing bundle, so a
  # failed build leaves the last working app in place instead of an empty folder.
  local bin="$BUILD_DIR/$EXECUTABLE-arm64"
  mkdir -p "$BUILD_DIR"
  swiftc -O -parse-as-library -sdk "$SDK" -target "arm64-apple-macosx$DEPLOYMENT_TARGET" \
    -o "$bin" "${APP_SOURCES[@]}"

  rm -rf "$APP_DIR"
  mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"
  mv "$bin" "$APP_DIR/Contents/MacOS/$EXECUTABLE"

  cp -R FLXFilmLab/Resources/packs "$APP_DIR/Contents/Resources/"
  cp FLXFilmLab/Resources/AppIcon.icns "$APP_DIR/Contents/Resources/" 2>/dev/null || \
    echo "    (no AppIcon.icns; run tools/icon/make_icon.py)"
  mkdir -p "$APP_DIR/Contents/Resources/Shaders"
  cp FLXFilmLab/Resources/Shaders/flx.metal FLXFilmLab/Motion/Resources/Shaders/motion.metal \
     "$APP_DIR/Contents/Resources/Shaders/"
  cp NOTICE.md LICENSE "$APP_DIR/Contents/Resources/" 2>/dev/null || true

  cat > "$APP_DIR/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleExecutable</key><string>$EXECUTABLE</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>$DEPLOYMENT_TARGET</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Camera RAW</string>
      <key>CFBundleTypeRole</key><string>Editor</string>
      <key>LSItemContentTypes</key>
      <array>
        <string>public.camera-raw-image</string>
        <string>com.adobe.raw-image</string>
        <string>com.nikon.raw-image</string>
        <string>public.image</string>
      </array>
    </dict>
    <dict>
      <key>CFBundleTypeName</key><string>Movie</string>
      <key>CFBundleTypeRole</key><string>Viewer</string>
      <key>LSItemContentTypes</key>
      <array>
        <string>public.movie</string>
        <string>com.apple.quicktime-movie</string>
        <string>public.mpeg-4</string>
      </array>
    </dict>
  </array>
</dict>
</plist>
PLIST

  echo "APPL????" > "$APP_DIR/Contents/PkgInfo"
  # Copied resources carry extended attributes that codesign rejects outright.
  xattr -cr "$APP_DIR"
  codesign --force --deep --sign - "$APP_DIR" 2>/dev/null || \
    echo "    (ad-hoc signing skipped; the linker signature still lets it run)"
  echo "==> $(pwd)/$APP_DIR"
  echo "    double-click it there, or drag it to /Applications"
}

build_motionapptest() {
  build_core
  echo "==> building app behaviour checks"
  swiftc_core -o "$BUILD_DIR/motionapptest" tests/motion/TestClip.swift tests/motionapp/main.swift
  echo "==> running app behaviour checks"
  "./$BUILD_DIR/motionapptest"
}

build_dmg() {
  build_app
  echo "==> packaging $APP_NAME.dmg"
  local stage="$BUILD_DIR/dmg"
  rm -rf "$stage"; mkdir -p "$stage"
  cp -R "$APP_DIR" "$stage/"
  ln -s /Applications "$stage/Applications"

  # Nothing here is signed with a paid Apple certificate, so macOS will refuse
  # the first launch on another machine. Ship the fix alongside the app rather
  # than leaving whoever opens it to guess.
  cat > "$stage/Read me first.txt" <<'NOTE'
FLX Film Lab
============

Install
-------
Drag "FLX Film Lab" onto the Applications folder in this window.

First launch
------------
macOS will refuse to open it the first time, with a message about the app being
damaged or from an unidentified developer. It is neither — the app just is not
signed with a paid Apple developer certificate, and macOS treats anything
downloaded without one this way.

To get past it, do this ONCE:

  1. Open Terminal (Applications > Utilities > Terminal).
  2. Paste this line and press Return:

       xattr -cr "/Applications/FLX Film Lab.app"

  3. Open the app normally from Applications.

That command removes the "downloaded from the internet" flag from the app. It
does not change the app itself.

What it does
------------
Open a camera RAW file (Nikon NEF, Leica DNG, and the other formats macOS
reads), pick a film stock and a paper, and it renders the photograph the way
that film would have rendered the light — using real spectral measurements from
the manufacturers' datasheets, not a filter or a preset.

Drop a RAW onto the window, or use Open.

Requires macOS 26 or later, on a Mac with Apple Silicon.

FLX Film Lab is free software under the GNU GPL v3. Film modelling powered by
spektrafilm (github.com/andreavolpato/spektrafilm); darkroom controls after
NegPy (github.com/marcinz606/NegPy). See NOTICE.md inside the app for full
attribution.
NOTE

  cp NOTICE.md LICENSE "$stage/" 2>/dev/null || true

  rm -f "$BUILD_DIR/$APP_NAME.dmg"
  hdiutil create -volname "$APP_NAME" -srcfolder "$stage" \
                 -ov -format UDZO -quiet "$BUILD_DIR/$APP_NAME.dmg"
  rm -rf "$stage"
  echo "==> $(pwd)/$BUILD_DIR/$APP_NAME.dmg  ($(du -h "$BUILD_DIR/$APP_NAME.dmg" | cut -f1))"
}

case "${1:-app}" in
  parity)  build_parity ;;
  checks)  build_checks ;;
  apptest) build_apptest ;;
  lut)     build_lut ;;
  lutchecks) build_lutchecks ;;
  motiontest) build_motiontest ;;
  motionapptest) build_motionapptest ;;
  motion|motionrun)
           echo "The Motion mode is part of FLX Film Lab (the toolbar's switch, or ⌘2)."
           build_app; [[ "$1" == motionrun ]] && open "$APP_DIR" ;;
  render)  build_render ;;
  strict)  out="$(build_strict)"; echo "$out"
           if grep -q "error:" <<< "$out"; then exit 1; fi
           echo "==> every source typechecks as GitHub's compiler would" ;;
  dmg)     build_dmg ;;
  run)     build_app; open "$APP_DIR" ;;
  install) refuse_if_running "/Applications/$APP_DIR" "$EXECUTABLE"
           build_app; rm -rf "/Applications/$APP_NAME.app"
           cp -R "$APP_DIR" /Applications/
           echo "==> /Applications/$APP_NAME.app"; open "/Applications/$APP_NAME.app" ;;
  *)      build_app ;;
esac
