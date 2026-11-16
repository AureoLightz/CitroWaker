#!/usr/bin/env bash
# Builds ww3ds.3dsx: the decompiled game (SwitchWaker's game/ and native/, Aurora's GX SDK) cross-compiled with clang
# for the 3DS ARM11, the 3DS platform layer (src/), and the GX -> PICA renderer (~/Downloads/ww-replay3ds/hw6) built
# with devkitARM g++. Incremental: a unit is rebuilt when its source is newer than its object (touch a source after a
# header change). Log: build.log.
#   bash build.sh            build
#   bash build.sh relink     only link
set -eu
here=$(cd "$(dirname "$0")" && pwd); cd "$here"
SW=$HOME/ww-port/SwitchWaker
# (Claude, release prep) N3DS_PUBLIC=1: asset-free build. Compiles the isolated tree made by public/make_tree.sh (stub asset headers,
# COS_RUNTIME_ASSETS=1: the game's data arrays are read from the player's ISO at start) into its own objects and outputs under
# public-build/ (bind-mounted over obj/, obj3ds/, all.sh, ww3ds.elf/.3dsx in the container), so public and private objects never mix.
PUB=${N3DS_PUBLIC:-0}
PUBMOUNTS=()
if [ "$PUB" = 1 ]; then
  SW=$HOME/ww-port/public/SwitchWaker
  [ -f "$SW/.public-tree-stamp" ] || { echo "no public tree: run bash public/make_tree.sh"; exit 1; }
  P=$here/public-build; mkdir -p "$P/obj" "$P/obj3ds" "$P/log"
  for f in all.sh todo.sh ww3ds.elf ww3ds.3dsx link.log; do [ -e "$P/$f" ] || : > "$P/$f"; done
  for d in obj obj3ds log; do PUBMOUNTS+=(-v "$P/$d:/arm/$d:z"); done
  for f in all.sh todo.sh ww3ds.elf ww3ds.3dsx link.log; do PUBMOUNTS+=(-v "$P/$f:/arm/$f:z"); done
fi
HW6=$HOME/Downloads/ww-replay3ds/hw6
mode=${1:-all}
# one build at a time (Claude and ChatGPT both build into obj/, obj3ds/ and ww3ds.elf; see COORDINATION.md)
exec 7>/tmp/ww3ds-build.lock; echo "waiting for the build lock..." >&2; flock 7
exec > >(tee build.log) 2>&1

if [ "$mode" != relink ]; then
  if [ "$PUB" = 1 ]; then SW_ROOT=$SW N3DS_PUBLIC=1 python3 mkcmds.py '/work/(game|native)/|aurora-src/lib/|recompcore' public-build/all.sh
  else python3 mkcmds.py '/work/(game|native)/|aurora-src/lib/|recompcore' all.sh; fi
  # generated sources: Aurora's FIFO with its processing mode chosen at run time (n3dsFifoThreadMode: performance.ini fifo_thread; Thread = Aurora's
  # worker, which the pthread wrapper puts on core 1; Inline = no worker) and a 6 MiB async-buffer drain limit (64 MiB on PC), GXFrameBuffer.cpp without the WebGPU copy path
  mkdir -p gen
  A=$SW/build/native-linux/_deps/aurora-src/lib
  sed -e 's/^constexpr auto kProcessingMode = ProcessingMode::Thread;/}  \/\/ namespace\nextern "C" int n3dsFifoThreadMode(void);\nnamespace {\n#define kProcessingMode (n3dsFifoThreadMode() ? ProcessingMode::Thread : ProcessingMode::Inline)/' \
      -e 's/^constexpr uint32_t kAsyncBufferLimit = 64u << 20;/constexpr uint32_t kAsyncBufferLimit = 6u << 20;/' $A/gx/fifo.cpp > gen/fifo_3ds.cpp.new
  grep -q 'n3dsFifoThreadMode() ? ProcessingMode::Thread' gen/fifo_3ds.cpp.new && grep -q 'kAsyncBufferLimit = 6u << 20' gen/fifo_3ds.cpp.new || { echo "fifo_3ds.cpp generation failed"; exit 1; }
  # (Claude v96) the command buffer never grows past 1 MiB: past that (or when realloc fails) the game thread waits for the FIFO worker to
  # finish the buffered commands and starts the buffer over (v95 died on a failed 1->2 MiB realloc with the heap nearly full)
  python3 - gen/fifo_3ds.cpp.new <<'EOF2'
import sys
p = sys.argv[1]; s = open(p).read()
old = '''  const auto grow = [newCapacity] {
    auto* resized = static_cast<uint8_t*>(realloc(detail::sBufferData, newCapacity));
    AURORA_ASSERT(resized != nullptr, "fifo::write_data: failed to allocate {} bytes", newCapacity);
    detail::sBufferData = resized;
  };
  if (sWorkerThread.joinable()) {
    std::lock_guard lock{sBufferMutex};
    grow();
  } else {
    grow();
  }
'''
new = '''  if (!detail::sInDisplayList && needed64 > (1u << 20) && detail::sBufferSize > 0) {
    drain();   // 3DS: wait for the worker instead of growing; drain() starts the buffer over
    n3dsFifoDrainStalls++;
    if (length <= detail::sBufferCapacity) {
      std::memcpy(detail::sBufferData, data, length);
      detail::sBufferSize = length;
      return;
    }
    return write_data_grow(data, length);
  }
  const auto grow = [newCapacity] {
    auto* resized = static_cast<uint8_t*>(realloc(detail::sBufferData, newCapacity));
    if (resized != nullptr) detail::sBufferData = resized;
    return resized != nullptr;
  };
  bool ok;
  if (sWorkerThread.joinable()) {
    std::lock_guard lock{sBufferMutex};
    ok = grow();
  } else {
    ok = grow();
  }
  if (!ok && !detail::sInDisplayList && detail::sBufferSize > 0) {
    drain();
    n3dsFifoDrainStalls++;
    if (length <= detail::sBufferCapacity) {
      std::memcpy(detail::sBufferData, data, length);
      detail::sBufferSize = length;
      return;
    }
  }
  AURORA_ASSERT(ok, "fifo::write_data: failed to allocate {} bytes", newCapacity);
'''
assert s.count(old) == 1, 'fifo grow patch: anchor missing'
s = s.replace(old, new)
s = s.replace('void write_data_grow(const void* data, uint32_t length) {', 'extern "C" { unsigned n3dsFifoDrainStalls = 0; }\nvoid write_data_grow(const void* data, uint32_t length) {', 1)
# (Claude v115) cheap worker wake: the game thread published after every draw (kDrawBatchSize 1) and each publish called notify_all, which
# here is a mutex + condition-variable broadcast (kernel calls) whenever the worker waits (often: it catches up). The worker now raises
# sWorkerSleeping before it blocks; wake_worker notifies only when it is set (seq_cst on both sides: no lost wake-up).
s = s.replace('std::atomic<uint32_t> sWorkerWake{0};', 'std::atomic<uint32_t> sWorkerWake{0};\nstd::atomic<bool> sWorkerSleeping{false};', 1)
old = """  sWorkerWake.fetch_add(1, std::memory_order_release);
  sWorkerWake.notify_all();"""
new = """  sWorkerWake.fetch_add(1, std::memory_order_seq_cst);
  if (sWorkerSleeping.load(std::memory_order_seq_cst)) sWorkerWake.notify_all();"""
assert s.count(old) == 1, 'fifo wake patch: anchor missing'
s = s.replace(old, new)
old = """    profile::set_idle(true);
    sWorkerWake.wait(event, std::memory_order_acquire);
    profile::set_idle(false);"""
new = """    profile::set_idle(true);
    sWorkerSleeping.store(true, std::memory_order_seq_cst);
    if (sWorkerWake.load(std::memory_order_seq_cst) == event) sWorkerWake.wait(event, std::memory_order_acquire);
    sWorkerSleeping.store(false, std::memory_order_relaxed);
    profile::set_idle(false);"""
assert s.count(old) == 1, 'fifo sleep patch: anchor missing'
s = s.replace(old, new)
# same for the worker's notifies (sProcessed after every processed chunk, sFramesDone, sJobsDone): only when a game-side waiter is counted
s = s.replace('std::atomic<bool> sWorkerSleeping{false};', 'std::atomic<bool> sWorkerSleeping{false};\nstd::atomic<int> sGameWaiters{0};', 1)
for name in ('sProcessed', 'sFramesDone', 'sJobsDone'):
    o = '    %s.notify_all();' % name if name != 'sJobsDone' else '  sJobsDone.notify_all();'
    ind = o[:len(o) - len(o.lstrip())]
    assert s.count(o) >= 1, 'fifo notify patch: ' + name
    s = s.replace(o, ind + 'std::atomic_thread_fence(std::memory_order_seq_cst);\n' + ind + 'if (sGameWaiters.load(std::memory_order_seq_cst) > 0) ' + o.lstrip())
old = '''    counter.wait(value, std::memory_order_acquire);
    value = counter.load(std::memory_order_acquire);'''
new = '''    sGameWaiters.fetch_add(1, std::memory_order_seq_cst);
    if (counter.load(std::memory_order_seq_cst) == value) counter.wait(value, std::memory_order_acquire);
    sGameWaiters.fetch_sub(1, std::memory_order_relaxed);
    value = counter.load(std::memory_order_acquire);'''
assert s.count(old) == 1, 'fifo wait_at_least patch'
s = s.replace(old, new)
old = '''        sProcessed.wait(processed, std::memory_order_acquire);
        processed = sProcessed.load(std::memory_order_acquire);'''
new = '''        sGameWaiters.fetch_add(1, std::memory_order_seq_cst);
        if (sProcessed.load(std::memory_order_seq_cst) == processed) sProcessed.wait(processed, std::memory_order_acquire);
        sGameWaiters.fetch_sub(1, std::memory_order_relaxed);
        processed = sProcessed.load(std::memory_order_acquire);'''
assert s.count(old) == 1, 'fifo drain wait patch'
s = s.replace(old, new)
open(p, 'w').write(s)
EOF2
  python3 - "$A/dolphin/gx/GXFrameBuffer.cpp" gen/GXFrameBuffer_3ds.cpp.new <<'EOF'
import sys
s = open(sys.argv[1]).read()
a = s.index('namespace {\naurora::Vec2<uint32_t> scale_copy_dst')
b = s.index('} // namespace aurora::gx\n') + len('} // namespace aurora::gx\n')
open(sys.argv[2], 'w').write(s[:a] + '// 3DS build: EFB copies are made by the renderer from the GX stream (GX_AURORA copy commands)\n' + s[b:])
EOF
  # Item B: generated 3DS SDK headers/sources; shared Aurora stays untouched.
  python3 gen_typed_gx.py "$A" gen/typed_gx
  for f in fifo_3ds.cpp GXFrameBuffer_3ds.cpp; do cmp -s gen/$f.new gen/$f || mv gen/$f.new gen/$f; rm -f gen/$f.new; done
fi

podman run --rm --memory=3g --memory-swap=3g --cpus=4 -e N3DS_RENDER_OPT -e N3DS_RENDER_LTO -e N3DS_PGO -e N3DS_EVENT_INDIRECT -v "$SW:/work:z" -v "$here:/arm:z" "${PUBMOUNTS[@]}" -v "$HW6:/hw6:z" localhost/ww3ds-clang bash -c '
set -e
mode='"$mode"'
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
mkdir -p /arm/obj /arm/obj3ds /arm/log
if [ "$mode" != relink ]; then
  # ---- 1. the game, native/ and Aurora (clang), from compile_commands.json
  : > /arm/todo.sh
  # (Claude v111) profile-guided game build: N3DS_PGO=gen (instrumented; src/pgo_runtime_3ds.cpp dumps the counters) or use (pgo/ww.profdata).
  # Changing the mode (or the profile) rebuilds every game unit.
  pgo=""
  case "${N3DS_PGO:-}" in
    gen) pgo="-fprofile-generate -mllvm -disable-vp=true" ;;
    use) pgo="-fprofile-use=/arm/pgo/ww.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing" ;;
  esac
  pgosum=""; if [ "${N3DS_PGO:-}" = use ]; then pgosum=$(md5sum /arm/pgo/ww.profdata | cut -c1-32); fi
  pgostamp="${N3DS_PGO:-none} $pgosum"
  pgoforce=false; [ "$(cat /arm/obj/.pgo-stamp 2>/dev/null)" = "$pgostamp" ] || pgoforce=true
  echo "pgo: mode ${N3DS_PGO:-none} rebuild-all $pgoforce"
  while IFS= read -r line; do
    line="$line $pgo"
    o=$(echo "$line" | grep -o "\-o [^ ]*" | cut -c4-)
    s=$(echo "$line" | grep -o "\-c [^ ]*" | cut -c4-)
    grep -qx "$(basename "$o")" /arm/exclude.txt && continue
    # J3D producer hook: OFF at runtime unless j3d_programs=1.
    case "$s" in
      /work/game/src/JSystem/J3DGraphBase/J3DShape.cpp|/work/game/src/JSystem/J3DGraphBase/J3DPacket.cpp)
        line="$line -DN3DS_J3D_DIRECT_PRODUCER_V1"
        if [ /work/game/src/JSystem/J3DGraphBase/J3DDirect3ds.inc -nt "$o" ] || [ /work/game/include/JSystem/J3DGraphBase/J3DDirect3ds.h -nt "$o" ]; then typed_dep=true; fi ;;
    esac
    # Item B: same output objects, generated SDK input and explicit header deps.
    typed_dep=false
    case "$s" in
      /work/build/native-linux/_deps/aurora-src/lib/dolphin/gx/*.cpp)
        replacement=/arm/gen/typed_gx/$(basename "$s")
        if [ -f "$replacement" ]; then
          line=$(echo "$line" | sed "s|$s|$replacement|"); s=$replacement
          if [ /arm/gen/typed_gx/__gx.h -nt "$o" ] || [ /arm/src/typed_gx_emit_3ds.h -nt "$o" ] || [ /arm/src/typed_gx_api_3ds.h -nt "$o" ]; then typed_dep=true; fi
        fi ;;
    esac
    if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || $typed_dep || $pgoforce; then echo "$line" >> /arm/todo.sh; fi
  done < /arm/all.sh
  n=$(wc -l < /arm/todo.sh); echo "game units to build: $n"
  rm -f /arm/log/*
  if [ "$n" -gt 0 ]; then
    seq 1 "$n" | xargs -P 4 -n 1 bash /arm/runone.sh
    nerr=$(ls /arm/log/*.err 2>/dev/null | wc -l); echo "game units failed: $nerr"
    [ "$nerr" = 0 ] && echo "$pgostamp" > /arm/obj/.pgo-stamp
    for e in /arm/log/*.err; do [ -f "$e" ] || continue; tail -1 "$e" | grep -o "\-c [^ ]*"; grep -m3 "error" "$e"; done
    [ "$nerr" -eq 0 ] || exit 1
  fi

  # ---- 2. the 3DS platform layer, game side (clang)
  base=$(grep "lib/gx/fifo.cpp" /arm/all.sh | sed "s/ -o .*//")
  A=/work/build/native-linux/_deps/aurora-src
  extra="-I$A/lib -iquote $A/lib/gx -iquote $A/lib/dolphin/gx -I/work/build/native-linux/_deps/nod_prebuilt-src/include -I/arm/src -I/work/native/include/sdk"
  pids=""
  for s in /arm/src/aurora_3ds.cpp /arm/src/sdl_3ds.cpp /arm/src/nod_3ds.cpp /arm/src/pad_3ds.cpp /arm/gen/fifo_3ds.cpp /arm/gen/GXFrameBuffer_3ds.cpp /arm/src/origin_prof_3ds.cpp /work/build/native-linux/_deps/fmt-src/src/format.cc; do
    o=/arm/obj3ds/c_$(basename $s).o
    if [ ! -f $o ] || [ $s -nt $o ] || [ /arm/src/n3ds.h -nt $o ]; then
      ( eval "$base $extra -o $o -c $s -w" > /arm/log/c_$(basename $s).log 2>&1 || { echo "FAILED $s"; grep -m8 error /arm/log/c_$(basename $s).log; } ) &
      pids="$pids $!"
      if [ "$(jobs -rp | wc -l)" -ge 4 ]; then wait -n; fi
    fi
  done
  pcbase=$(grep "native/src/pc/runtime/pc_frame.cpp" /arm/all.sh | sed "s/ -o .*//")
  for s in /arm/src/pcstubs_3ds.cpp; do
    o=/arm/obj3ds/c_$(basename $s).o
    if [ ! -f $o ] || [ $s -nt $o ]; then
      ( eval "$pcbase -I/work/native/src -o $o -c $s -w" > /arm/log/c_$(basename $s).log 2>&1 || { echo "FAILED $s"; grep -m8 error /arm/log/c_$(basename $s).log; } ) &
      pids="$pids $!"
      if [ "$(jobs -rp | wc -l)" -ge 4 ]; then wait -n; fi
    fi
  done
  for p in $pids; do wait $p; done

  # ---- 3. the renderer and the 3DS side (devkitARM g++)
  GXX=$DEVKITARM/bin/arm-none-eabi-g++
  F="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
  RENDER_OPT=${N3DS_RENDER_OPT:-2}
  case "$RENDER_OPT" in 2|3) ;; *) echo "N3DS_RENDER_OPT must be 2 or 3" >&2; exit 1 ;; esac
  CXXF="-std=gnu++20 -O$RENDER_OPT -DN3DS_RENDER_OPT=$RENDER_OPT -mword-relocations -ffunction-sections $F -D__3DS__ -I$DEVKITPRO/libctru/include -I/hw6/src -I/hw6/src3ds -I/hw6 -I/arm/src"
  EVENT_INDIRECT=${N3DS_EVENT_INDIRECT:-0}
  case "$EVENT_INDIRECT" in 0|1) ;; *) echo "N3DS_EVENT_INDIRECT must be 0 or 1" >&2; exit 1 ;; esac
  CXXF="$CXXF -DN3DS_EVENT_INDIRECT=$EVENT_INDIRECT"
  rm -f /arm/log/.renderer_failed
  RENDER_LTO=${N3DS_RENDER_LTO:-0}
  case "$RENDER_LTO" in 0|1) ;; *) echo "N3DS_RENDER_LTO must be 0 or 1" >&2; exit 1 ;; esac
  CXXF="$CXXF -DN3DS_RENDER_LTO=$RENDER_LTO"
  if [ "$RENDER_LTO" = 1 ]; then CXXF="$CXXF -flto=2 -fno-fat-lto-objects"; fi
  flags_changed=0
  [ -f /arm/obj3ds/.cxxflags ] && [ "$(cat /arm/obj3ds/.cxxflags)" = "$CXXF" ] || flags_changed=1
  pids=""
  newest_h=$(ls -t /hw6/src/*.h /hw6/src3ds/*.h /arm/src/*.h | head -1)
  for s in /hw6/src/*.cpp /hw6/src3ds/*.cpp /arm/src/gpu_3ds.cpp /arm/src/main_3ds.cpp /arm/src/audio_3ds.cpp /arm/src/render_thread_3ds.cpp /arm/src/typed_gx_receiver_3ds.cpp /arm/src/model_material_3ds.cpp; do
    o=/arm/obj3ds/g_$(basename $s .cpp).o
    if [ "$flags_changed" = 1 ] || [ ! -f $o ] || [ $s -nt $o ] || [ $newest_h -nt $o ]; then
      ( $GXX $CXXF -c $s -o $o > /arm/log/g_$(basename $s).log 2>&1 || { echo "FAILED $s"; grep -m8 error /arm/log/g_$(basename $s).log; touch /arm/log/.renderer_failed; } ) &
      pids="$pids $!"
      if [ "$(jobs -rp | wc -l)" -ge 4 ]; then wait -n; fi
    fi
  done
  # (Claude v77) citro3d 1.7.1 from source (n3ds/citro3d: the release the container links, same functions and data sizes), with two exact
  # changes: a 4-flags-at-a-time clean-uniform scan and C3Di_DrawElementsUpdated (no second context update). These objects replace -lcitro3d.
  for s in /arm/citro3d/source/*.c /arm/citro3d/source/maths/*.c; do
    o=/arm/obj3ds/c3d_$(basename $s .c).o
    if [ ! -f $o ] || [ $s -nt $o ] || [ /arm/citro3d/source/internal.h -nt $o ]; then
      ( $DEVKITARM/bin/arm-none-eabi-gcc -O2 -Wall -Wno-sizeof-array-div -mword-relocations -ffunction-sections -fdata-sections $F -I/arm/citro3d/include -I$DEVKITPRO/libctru/include -D__3DS__ -DCITRO3D_BUILD -c $s -o $o > /arm/log/c3d_$(basename $s).log 2>&1 || { echo "FAILED $s"; grep -m8 error /arm/log/c3d_$(basename $s).log; touch /arm/log/.renderer_failed; } ) &
      pids="$pids $!"
      if [ "$(jobs -rp | wc -l)" -ge 4 ]; then wait -n; fi
    fi
  done
  # game-thread profiler (game/include/prof_game.h; enabled at run time by N3DS_PROF_GAME=1 in env.txt)
  o=/arm/obj3ds/g_prof_game.o
  if [ "$flags_changed" = 1 ] || [ ! -f $o ] || [ /arm/src/prof_game.cpp -nt $o ] || [ /work/game/include/prof_game.h -nt $o ]; then
    ( $GXX $CXXF -I/work/game/include -c /arm/src/prof_game.cpp -o $o > /arm/log/g_prof_game.log 2>&1 || { echo "FAILED prof_game"; grep -m8 error /arm/log/g_prof_game.log; touch /arm/log/.renderer_failed; } ) &
    pids="$pids $!"
    if [ "$(jobs -rp | wc -l)" -ge 4 ]; then wait -n; fi
  fi
  for p in $pids; do wait $p; done
  if [ -f /arm/log/.renderer_failed ]; then exit 1; fi
  printf "%s\n" "$CXXF" > /arm/obj3ds/.cxxflags
fi

# ---- 4. link
cd /arm/obj
LINKOPT=""
if [ -f /arm/obj3ds/.cxxflags ] && grep -q -- "-flto=2" /arm/obj3ds/.cxxflags; then LINKOPT="-O3 -flto=2"; fi
objs=$(grep -o "\-o /arm/obj/[^ ]*" /arm/all.sh | sed "s|-o /arm/obj/||" | grep -v -x -f /arm/exclude.txt | tr "\n" " ")
$DEVKITARM/bin/arm-none-eabi-g++ $LINKOPT -specs=3dsx.specs -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -Wl,--no-enum-size-warning -Wl,--gc-sections -Wl,--wrap=__syscall_thread_create -Wl,--wrap=malloc -Wl,--wrap=__libc_lock_acquire_recursive -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=aligned_alloc -Wl,--wrap=free -Wl,--wrap=GDInitGDLObj -Wl,--wrap=GXCallDisplayList -Wl,--wrap=GXBegin -Wl,--wrap=GXDrawDone -Wl,--wrap=_ZN6aurora2gx4fifo5drainEv -Wl,--wrap=_ZN6aurora2gx4fifo4syncEv -Wl,--wrap=GX_BindQueue -Wl,--wrap=GX_ProcessCommandList \
  $objs /arm/obj3ds/*.o -L$DEVKITPRO/libctru/lib -L$DEVKITPRO/portlibs/3ds/lib -lcitro3d -lctru -lz -lm -o /arm/ww3ds.elf 2> /arm/link.log || true
if [ -f /arm/ww3ds.elf ] && ! grep -q "undefined reference\|error:" /arm/link.log; then
  $DEVKITPRO/tools/bin/3dsxtool /arm/ww3ds.elf /arm/ww3ds.3dsx --smdh=/arm/ww3ds.smdh 2>/dev/null || $DEVKITPRO/tools/bin/3dsxtool /arm/ww3ds.elf /arm/ww3ds.3dsx
  ls -la /arm/ww3ds.3dsx
else
  echo "LINK FAILED: $(grep -c "undefined reference" /arm/link.log) undefined references"; grep -v "undefined reference" /arm/link.log | grep -m10 "error"
fi
'
