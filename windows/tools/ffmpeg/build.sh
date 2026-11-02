#!/usr/bin/env bash
# Builds the FFmpeg the Windows app carries: FFmpeg's own release, under the
# LGPL (configured without --enable-gpl or --enable-nonfree), with only what
# the app uses. That is the containers Motion opens, their video and audio
# decoders, the GPU's video decoders (Direct3D 11, DXVA2, NVIDIA's), ProRes
# and the GPU's HEVC encoders for export (NVIDIA's, AMD's and Intel's), the
# mov muxer, swscale and swresample.
# Cross-compiled with mingw-w64 into DLLs and their import libraries, which
# the app links and the installer carries beside it.
#
#   tools/ffmpeg/build.sh [OUT]        OUT defaults to build/ffmpeg
#
# Needs mingw-w64, nasm, make, CMake and pkg-config: Homebrew's on a Mac
# (brew install mingw-w64 nasm pkgconf), as the release workflow runs it on
# GitHub's macOS image. The sources are fetched once into build/ffmpeg-cache
# and checked against the hashes below. OUT/SOURCE.txt says where each came
# from and how it was built, as the LGPL asks of a library handed on.
#
# FFmpeg keeps its configure line in every DLL (avutil_configuration and the
# like), so it names no folder of the machine it is built on: FFmpeg
# installs into a staging folder of its own, and finds what it is built
# with by a relative path.

set -euo pipefail
cd "$(dirname "$0")/../.."
root="$(pwd -P)"
out="${1:-build/ffmpeg}"
[[ "$out" = /* ]] || out="$root/$out"
cache="$root/build/ffmpeg-cache"
work="$root/build/ffmpeg-work"

FFMPEG_VERSION=8.1.3
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
FFMPEG_SHA256=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
# NVIDIA's codec headers (MIT), the oldest FFmpeg 8.1 takes, for the widest
# range of drivers; headers only, the driver's own libraries load at run time.
NV_VERSION=12.1.14.1
NV_URL="https://github.com/FFmpeg/nv-codec-headers/releases/download/n$NV_VERSION/nv-codec-headers-$NV_VERSION.tar.gz"
NV_SHA256=043e46265517855f13bcb98b3a225b2761b2a83983b1645fa1d05876f1c3a781
# Intel's libvpl (MIT), the dispatcher FFmpeg's QSV encoder loads Intel's
# runtime through, built here and linked into FFmpeg.
VPL_VERSION=2.17.0
VPL_URL="https://github.com/intel/libvpl/archive/refs/tags/v$VPL_VERSION.tar.gz"
VPL_SHA256=4de3e2faf1e8307fb282e4a43f443191810f6a6b0a484fffa7995ba1c814c6ec
# AMD's AMF headers (MIT), v1.4.36, likewise the oldest FFmpeg 8.1 takes.
AMF_REPO=https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git
AMF_TAG=v1.4.36
AMF_COMMIT=16f7d73e0b45c473e903e46981ed0b91efc4c091
# The licence of mingw-w64's winpthreads, which the C++ runtime libvpl needs
# brings into avcodec and avutil, for the toolchain's mingw-w64 14.0.0.
WINPTHREADS_URL=https://raw.githubusercontent.com/mingw-w64/mingw-w64/v14.0.0/mingw-w64-libraries/winpthreads/COPYING
WINPTHREADS_SHA256=63263614cdd29f2f93cba85e992f041b31f9fc7b4033692f31269489a8a1b177

host=x86_64-w64-mingw32
for tool in "$host-gcc" "$host-g++" "$host-dlltool" nasm make cmake pkg-config curl git; do
  command -v "$tool" >/dev/null || { echo "error: $tool is missing (mingw-w64, nasm, make, pkg-config)" >&2; exit 1; }
done

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
fetch() {
  local file="$cache/$(basename "$1")"
  [[ -f "$file" ]] || curl -sSfL -o "$file" "$1"
  [[ "$(sha256 "$file")" == "$2" ]] || { echo "error: $file is not the file pinned here" >&2; exit 1; }
  echo "$file"
}

mkdir -p "$cache"
ffmpeg_tar="$(fetch "$FFMPEG_URL" "$FFMPEG_SHA256")"
nv_tar="$(fetch "$NV_URL" "$NV_SHA256")"
winpthreads_copying="$cache/winpthreads-COPYING-v14.0.0"
[[ -f "$winpthreads_copying" ]] || curl -sSfL -o "$winpthreads_copying" "$WINPTHREADS_URL"
[[ "$(sha256 "$winpthreads_copying")" == "$WINPTHREADS_SHA256" ]] || { echo "error: $winpthreads_copying is not the file pinned here" >&2; exit 1; }
vpl_tar="$cache/libvpl-$VPL_VERSION.tar.gz"
[[ -f "$vpl_tar" ]] || curl -sSfL -o "$vpl_tar" "$VPL_URL"
[[ "$(sha256 "$vpl_tar")" == "$VPL_SHA256" ]] || { echo "error: $vpl_tar is not the file pinned here" >&2; exit 1; }
if [[ "$(git -C "$cache/amf" rev-parse HEAD 2>/dev/null)" != "$AMF_COMMIT" ]]; then
  rm -rf "$cache/amf"
  git init -q "$cache/amf"
  git -C "$cache/amf" remote add origin "$AMF_REPO"
  git -C "$cache/amf" sparse-checkout set amf/public/include
  git -C "$cache/amf" fetch -q --depth 1 --filter=blob:none origin "$AMF_COMMIT"
  git -C "$cache/amf" checkout -q FETCH_HEAD
fi

rm -rf "$work"
mkdir -p "$work/prefix/include"
tar -xzf "$nv_tar" -C "$work"
make -s -C "$work/nv-codec-headers-$NV_VERSION" PREFIX="$work/prefix" install >/dev/null
cp -R "$cache/amf/amf/public/include" "$work/prefix/include/AMF"
tar -xzf "$vpl_tar" -C "$work"
cmake -S "$work/libvpl-$VPL_VERSION" -B "$work/libvpl-build" -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DCMAKE_C_COMPILER="$host-gcc" -DCMAKE_CXX_COMPILER="$host-g++" \
  -DCMAKE_RC_COMPILER="$host-windres" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_TOOLS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_EXAMPLES=OFF -DINSTALL_DEV=ON \
  -DCMAKE_INSTALL_PREFIX="$work/prefix" -DCMAKE_INSTALL_LIBDIR=lib > "$work/libvpl.log" 2>&1 &&
  cmake --build "$work/libvpl-build" -j "$(getconf _NPROCESSORS_ONLN)" >> "$work/libvpl.log" 2>&1 &&
  cmake --install "$work/libvpl-build" >> "$work/libvpl.log" 2>&1 ||
  { tail -20 "$work/libvpl.log"; echo "error: libvpl failed ($work/libvpl.log)" >&2; exit 1; }
tar -xJf "$ffmpeg_tar" -C "$work"

configure=(
  --arch=x86_64 --target-os=mingw32 --cross-prefix="$host-"
  --enable-cross-compile --pkg-config=pkg-config
  --enable-shared --disable-static --disable-programs --disable-doc --disable-debug
  --disable-autodetect --disable-everything --disable-network --disable-avdevice --disable-avfilter
  --enable-w32threads
  --enable-protocol=file
  --enable-demuxer=mov,mxf,matroska,avi,mpegts,mpegps
  --enable-muxer=mov
  --enable-decoder=hevc,h264,prores,dnxhd,mpeg2video,aac,ac3,eac3,mp2,mp3,alac,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_bluray,pcm_dvd
  --enable-parser=hevc,h264,mpegvideo,dnxhd,aac,ac3,mpegaudio
  --enable-encoder=prores_ks,hevc_nvenc,hevc_amf,hevc_qsv
  --enable-hwaccel=hevc_d3d11va,hevc_d3d11va2,hevc_dxva2,hevc_nvdec,h264_d3d11va,h264_d3d11va2,h264_dxva2,h264_nvdec
  --enable-d3d11va --enable-dxva2 --enable-ffnvcodec --enable-cuda --enable-nvdec --enable-nvenc --enable-amf
  --enable-libvpl
  --extra-cflags=-I../prefix/include --extra-ldflags=-static --extra-libs=-lstdc++
)
(cd "$work/ffmpeg-$FFMPEG_VERSION" &&
 PKG_CONFIG_PATH="$work/prefix/lib/pkgconfig" PKG_CONFIG_LIBDIR="$work/prefix/lib/pkgconfig" \
   ./configure "${configure[@]}" > "$work/configure.log" 2>&1 ||
   { tail -20 "$work/configure.log"; echo "error: configure failed ($work/configure.log)" >&2; exit 1; }
 make -s -j"$(getconf _NPROCESSORS_ONLN)" > "$work/make.log" 2>&1 ||
   { tail -20 "$work/make.log"; echo "error: make failed ($work/make.log)" >&2; exit 1; }
 make -s install DESTDIR="$work/stage" > /dev/null)
installed="$work/stage/usr/local"

# What the app links and carries: the DLLs, their import libraries, FFmpeg's
# own headers, the licence, and where it all came from.
rm -rf "$out"
mkdir -p "$out/bin" "$out/lib" "$out/include"
cp "$installed"/bin/*.dll "$out/bin/"
# the import libraries dlltool made from each .def, installed beside the DLLs
cp "$installed"/bin/*.lib "$out/lib/"
for d in libavcodec libavformat libavutil libswscale libswresample; do cp -R "$installed/include/$d" "$out/include/"; done
cp "$work/ffmpeg-$FFMPEG_VERSION/COPYING.LGPLv2.1" "$out/LICENSE.txt"
# what is compiled in besides FFmpeg, each with its own notice
{
  echo "What FLX Film Lab's FFmpeg build compiles in besides FFmpeg (LICENSE.txt)"
  echo
  echo "== Intel's libvpl $VPL_VERSION (MIT), linked into avcodec and avutil"
  echo
  cat "$work/libvpl-$VPL_VERSION/LICENSE"
  echo
  echo "== NVIDIA's codec headers $NV_VERSION (MIT), whose loader is compiled in"
  echo
  sed -n '1,/\*\//p' "$work/nv-codec-headers-$NV_VERSION/include/ffnvcodec/dynlink_loader.h"
  echo
  echo "== AMD's AMF headers $AMF_TAG (MIT)"
  echo
  cat "$cache/amf/LICENSE.txt"
  echo
  echo "== mingw-w64's winpthreads (MIT, and a BSD-style notice for a part), linked"
  echo "   into avcodec and avutil with the C++ runtime"
  echo
  cat "$winpthreads_copying"
  echo
  echo "== GCC's runtime (libstdc++, libgcc), linked into avcodec and avutil under the"
  echo "   GCC Runtime Library Exception, version 3.1, which permits it"
} > "$out/NOTICES.txt"
cat > "$out/SOURCE.txt" <<EOF
FFmpeg $FFMPEG_VERSION, as FLX Film Lab for Windows carries it

Licence: the GNU Lesser General Public License 2.1 or later (LICENSE.txt).
Configured without --enable-gpl or --enable-nonfree, so no GPL or non-free
part is in it.

Source: $FFMPEG_URL
  SHA-256 $FFMPEG_SHA256
Compiled in with it (NOTICES.txt has their licences):
  Intel's libvpl $VPL_VERSION (MIT), $VPL_URL
    SHA-256 $VPL_SHA256
  NVIDIA's codec headers $NV_VERSION (MIT), $NV_URL
    SHA-256 $NV_SHA256
  AMD's AMF headers $AMF_TAG (MIT), $AMF_REPO at $AMF_COMMIT
  mingw-w64's winpthreads and GCC's runtime, from the toolchain below

Built by tools/ffmpeg/build.sh in FLX Film Lab's repository (windows/),
https://github.com/felixjablonka/flx-film-lab, linked statically against the
toolchain's runtime so the DLLs need only Windows' own. Compiled with mingw-w64
($("$host-gcc" --version | head -1)), configured in FFmpeg's source folder,
beside prefix/, which holds libvpl and the NVIDIA and AMD headers:
  ./configure ${configure[*]}
EOF
echo "==> $out: FFmpeg $FFMPEG_VERSION, $(ls "$out/bin" | tr '\n' ' ')"
