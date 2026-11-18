#!/bin/bash
# Builds the CitroWaker Installer for Linux and Windows in containers (rootless podman; with docker add --user); no host toolchain needed.
#   [CIA=path/to/CitroWaker.cia] bash installer/build_release.sh [linux|windows|all]
# With CIA set (release builds), the game file is built into the installer: players download one file.
# Output: installer/dist/CitroWaker-Installer-linux-x86_64 and installer/dist/CitroWaker-Installer.exe
#   Linux:   Ubuntu 22.04 (glibc 2.35) + PyInstaller, mktexpack built with g++ (static libstdc++).
#   Windows: mktexpack.exe cross-built with MinGW-w64 (fully static), PyInstaller under Wine (tobix/pywine).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
OUT=$ROOT/installer/dist
mkdir -p "$OUT"
ENGINE=$(command -v podman || command -v docker)
WHAT=${1:-all}
TP=tools/texture-pack
SRCS="$TP/mktexpack.cpp $TP/src/texconv.cpp $TP/src/texfit.cpp $TP/src/texmap.cpp $TP/src/texpack.cpp $TP/src/texture_support.cpp"
PYI_DATA="--add-data /src/$TP/extract_textures.py:texture-pack --add-data /src/$TP/third_party:texture-pack/third_party"
WIN_CIA=""
if [ -n "${CIA:-}" ]; then
  mkdir -p installer/build-cia && cp "$CIA" installer/build-cia/CitroWaker.cia
  PYI_DATA="$PYI_DATA --add-data /src/installer/build-cia/CitroWaker.cia:."
  WIN_CIA="--add-data 'Z:\\src\\installer\\build-cia\\CitroWaker.cia;.'"
fi

if [ "$WHAT" = linux ] || [ "$WHAT" = all ]; then
  "$ENGINE" run --rm -v "$ROOT:/src:z" -w /src docker.io/library/ubuntu:22.04 bash -euc "
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq && apt-get install -y -qq python3 python3-pip python3-tk g++ binutils >/dev/null
    pip3 install -q pyinstaller==6.11.1 certifi==2024.8.30
    mkdir -p /tmp/b && g++ -std=c++17 -O2 -pthread -static-libstdc++ -static-libgcc -I$TP/src $SRCS -o /tmp/b/mktexpack
    pyinstaller --noconfirm --clean --onefile --windowed --name CitroWaker-Installer-linux-x86_64 \
      --distpath /src/installer/dist --workpath /tmp/b/work --specpath /tmp/b \
      --add-binary /tmp/b/mktexpack:. --collect-data certifi $PYI_DATA /src/installer/citrowaker_installer.py >/dev/null"
  echo "built $OUT/CitroWaker-Installer-linux-x86_64"
fi

if [ "$WHAT" = windows ] || [ "$WHAT" = all ]; then
  "$ENGINE" run --rm -v "$ROOT:/src:z" -w /src registry.fedoraproject.org/fedora:44 bash -euc "
    dnf install -y -q mingw64-gcc-c++ mingw64-winpthreads-static >/dev/null
    mkdir -p /src/installer/build-win
    x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -I$TP/src $SRCS -o /src/installer/build-win/mktexpack.exe
    true"
  "$ENGINE" run --rm -v "$ROOT:/src:z" -w /src docker.io/tobix/pywine:3.12 bash -euc "
    wine python -m pip install -q pyinstaller==6.11.1 certifi==2024.8.30 2>/dev/null
    wine pyinstaller --noconfirm --clean --onefile --windowed --name CitroWaker-Installer \
      --distpath Z:\\\\src\\\\installer\\\\dist --workpath Z:\\\\tmp\\\\w --specpath Z:\\\\tmp\\\\w \
      --add-binary 'Z:\\src\\installer\\build-win\\mktexpack.exe;.' \
      --add-data 'Z:\\src\\$TP\\extract_textures.py;texture-pack' --add-data 'Z:\\src\\$TP\\third_party;texture-pack\\third_party' \
      $WIN_CIA --collect-data certifi installer/citrowaker_installer.py 2>&1 | tail -3"
  echo "built $OUT/CitroWaker-Installer.exe"
fi
