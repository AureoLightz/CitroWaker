#!/usr/bin/env bash
# Builds benchreplay3ds.3dsx from benchreplay3ds.cpp + the hw6 ARM objects of the last n3ds build.sh
# (~/ww-port/n3ds/obj3ds/g_*.o). Run n3ds/build.sh first after changing hw6 sources.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out=${OUT:-$here/../bench_out}; mkdir -p "$out"
exec 7>/tmp/ww3ds-build.lock; flock 7
podman run --rm -v "$here:/hw6:ro" -v "$HOME/ww-port/n3ds:/arm:ro" -v "$out:/out:z" localhost/ww3ds-clang bash -c '
set -eu
GXX=/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++
F="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
$GXX -std=gnu++20 -O2 $F -D__3DS__ -I/opt/devkitpro/libctru/include -I/hw6/src -I/hw6/src3ds -c /hw6/benchreplay3ds.cpp -o /out/benchreplay3ds.o
objs="$(ls /arm/obj3ds/c3d_*.o | tr "\n" " ")"; for s in /hw6/src/*.cpp /hw6/src3ds/*.cpp; do objs="$objs /arm/obj3ds/g_$(basename "$s" .cpp).o"; done
$GXX -specs=3dsx.specs $F -Wl,--gc-sections -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=free /out/benchreplay3ds.o $objs -L/opt/devkitpro/libctru/lib -lcitro3d -lctru -lm -o /out/benchreplay3ds.elf
/opt/devkitpro/tools/bin/3dsxtool /out/benchreplay3ds.elf /out/benchreplay3ds.3dsx
'
sha256sum "$out/benchreplay3ds.3dsx"
