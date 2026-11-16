#include <atomic>
// Aurora's core for the 3DS build (replaces lib/aurora.cpp, window.cpp, the WebGPU device, the GX command processor
// and the gfx back end): Aurora's GX SDK still writes its FIFO (lib/gx/fifo.cpp, built in Inline mode), and the
// command stream goes to the port's GX renderer (n3dsGpuProcess); the end of a frame draws it on the top screen.
#include <aurora/aurora.h>
#include <aurora/time.hpp>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>

#include "gx/command_processor.hpp"
#include "gx/fifo.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#include "n3ds.h"
#include "typed_gx_api_3ds.h"
extern "C" u64 svcGetSystemTick(void); // libctru SVC; clang units do not include 3ds.h
extern "C" void n3dsNextThreadStack(size_t bytes);   // gpu_3ds.cpp: stack size of the next pthread (Aurora's FIFO worker)

// pc_jkr_heap.cpp: while open, the global operator new gives host memory instead of a block of the game's current
// JKR heap. The renderer runs inside the game's GX calls, so without it its per-frame containers filled the main heap.
void JKRPcBeginHostAlloc();
void JKRPcEndHostAlloc();
namespace {
struct HostAlloc {
  HostAlloc() { JKRPcBeginHostAlloc(); }
  ~HostAlloc() { JKRPcEndHostAlloc(); }
};
}  // namespace

namespace aurora {
AuroraConfig g_config;
char g_gameName[5];
}  // namespace aurora

namespace {
AuroraEvent sEvents[2];
AuroraStats sStats{};
bool sQuit = false;

const char* dupOr(const char* s, const char* fallback) { return strdup(s != nullptr ? s : fallback); }
}  // namespace

// ---- the GX command processor: the port's renderer
namespace aurora::gx::fifo {
ProcessResult process(const uint8_t* data, uint32_t size) noexcept {
  int drawDone = 0;
  HostAlloc host;
  const uint32_t used = n3dsGpuProcess(data, size, &drawDone);
  return ProcessResult{used, drawDone != 0};
}
void clear_draw_cache() noexcept {}
void set_fast_paths(bool) noexcept {}
}  // namespace aurora::gx::fifo

namespace aurora::gfx::render_worker {
bool is_worker_thread() noexcept { return false; }
}  // namespace aurora::gfx::render_worker

namespace aurora::gfx::depth_peek {
extern "C" int n3dsReadClearDepth(uint16_t x, uint16_t y);
bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
  // Only a completed, actually clear depth sample is known exactly in both
  // Z- and W-buffer frames. Keep the old failure path for numeric scene depth.
  if (n3dsReadClearDepth(x, y)) { z = 0xFFFFFF; return true; }
  z = 0;
  return false;
}
}  // namespace aurora::gfx::depth_peek

namespace aurora::gx {
void set_viewport_policy(AuroraViewportPolicy) noexcept {}
}  // namespace aurora::gx

// ---- the window: the 3DS top screen, 400x240
namespace aurora::window {
AuroraWindowSize get_window_size() {
  return AuroraWindowSize{400, 240, 640, 480, 400, 240, 1.0f};
}
void set_title(const char*) {}
void set_fullscreen(bool) {}
bool get_fullscreen() { return true; }
void set_window_size(uint32_t, uint32_t) {}
void set_window_position(uint32_t, uint32_t) {}
void center_window() {}
void request_frame_buffer_resize() {}
void set_frame_buffer_scale(float) {}
void set_frame_buffer_aspect_fit(bool) {}
void set_frame_buffer_fit_aspect(float) {}
void set_background_input(bool) {}
}  // namespace aurora::window

extern "C" {

AuroraInfo aurora_initialize(int, char*[], const AuroraConfig* config) {
  aurora::g_config = *config;
  aurora::g_config.appName = dupOr(config->appName, "ww3ds");
  aurora::g_config.userPath = dupOr(config->userPath, N3DS_ROOT "/user");
  aurora::g_config.cachePath = dupOr(config->cachePath, N3DS_ROOT "/cache");
  aurora::g_config.resourcesPath = dupOr(config->resourcesPath, N3DS_ROOT);
  if (aurora::g_config.msaa == 0) aurora::g_config.msaa = 1;
  bool gpuOk;
  { HostAlloc host; gpuOk = n3dsGpuInit() != 0; }
  if (!gpuOk) {
    n3dsLog("[3ds] GPU set-up failed\n");
  }
  n3dsNextThreadStack(512u << 10);   // the FIFO worker's stack (only Thread mode creates one)
  aurora::gx::fifo::init();
  if (aurora::gx::fifo::processing_mode() != aurora::gx::fifo::ProcessingMode::Thread) n3dsNextThreadStack(0);
  // Thread mode (fifo_thread=1): Aurora's worker translates the FIFO (recorder: GX parse + state/vertex decode) on core 1 while the game runs on core 0, and
  // the end of a frame (texture snapshots, hand-off to the core-2 renderer) runs as the worker's frame job, the game at most one frame ahead (Aurora's
  // async end of frame, as the PC build's 60 fps mode).
  if (aurora::gx::fifo::processing_mode() == aurora::gx::fifo::ProcessingMode::Thread) aurora::gx::fifo::set_async_end_frame(true);
  AuroraInfo info{};
  info.backend = BACKEND_NULL;
  info.userPath = aurora::g_config.userPath;
  info.cachePath = aurora::g_config.cachePath;
  info.window = reinterpret_cast<SDL_Window*>(1);   // pc_main checks that a window exists; nothing reads it
  info.windowSize = aurora::window::get_window_size();
  return info;
}

void aurora_shutdown() { aurora::gx::fifo::shutdown(); n3dsGpuShutdown(); }

const AuroraEvent* aurora_update() {
  int n = 0;
  if (!sQuit && !n3dsAptLoop()) sQuit = true;
  if (sQuit) sEvents[n++].type = AURORA_EXIT;
  sEvents[n].type = AURORA_NONE;
  return sEvents;
}

// (Claude v114) a frame left open by an unpainted tick (aurora_end_frame returned early) is continued, not begun again (a second
// begin_frame without its end corrupted the GX stream: truncated commands, runaway memory in the emulator)
static bool sFrameOpen = false;
bool aurora_begin_frame() {
  if (sFrameOpen) return true;
  sFrameOpen = true;
  aurora::gx::fifo::begin_frame();
  n3dsTypedBeginFrame();
  return true;
}

extern "C" unsigned long long svcGetSystemTick(void);
extern "C" void n3dsDrainCallersLog(void);   // origin_prof_3ds.cpp (Claude v97 diag)
static inline uint64_t n3dsTick() { return svcGetSystemTick(); }
extern "C" { extern unsigned long long g_n3dsDrainTicks, g_n3dsSyncTicks; extern unsigned g_n3dsDrainCalls, g_n3dsSyncCalls; }
extern "C" int n3dsPaintSkipped(void);
extern "C" int n3dsEarlyReleaseOn(void);
extern "C" void svcSleepThread(long long ns);
static uint32_t sEndSeq = 0;                       // game thread: end_frame_async calls
static std::atomic<uint32_t> sReleased{0};         // worker: frames whose snapshot is frozen
static bool sReleasedThisJob = false;              // worker only
extern "C" void n3dsPaintEnded(void);    // (Claude v120) gpu_3ds.cpp paint pacing: a painted frame was ended / handed to the render core
extern "C" void n3dsPaintHandedOff(void);
extern "C" void n3dsEndFrameRelease(void) { if (sReleasedThisJob) return; sReleasedThisJob = true; sReleased.fetch_add(1, std::memory_order_release); }
void aurora_end_frame() {
  // (Claude v114) frame skip: the main loop ends a frame every logic tick; an unpainted tick has nothing to present (v113 presented an
  // empty frame there: black flicker on every skipped frame). Its few GX bytes, if any, stay queued for the next painted frame.
  if (n3dsPaintSkipped()) return;
  sFrameOpen = false;
  n3dsPaintEnded();
  n3dsTypedEndFrame();
  if (aurora::gx::fifo::async_end_frame()) {
    // (Claude) DIAGNOSTIC N3DS_PROF_WAIT=1: how long the game thread sits in end_frame_async (waiting for the FIFO worker) per frame
    static const bool profWait = std::getenv("N3DS_PROF_WAIT") != nullptr;
    const uint64_t w0 = profWait ? n3dsTick() : 0;
    // (Claude v115) the game waits only until the worker has decoded this frame and frozen its snapshot (n3dsEndFrameRelease, called
    // by n3dsGpuEndFrame right after the freeze), not for the rest of the job: the hand-off to the render core (which waits for that
    // core to return its previous buffer, up to ~20 ms on the console while rendering is the bottleneck) and its acknowledgement.
    // performance.ini early_release=0 = old full sync.
    if (n3dsEarlyReleaseOn()) {
      const uint32_t seq = ++sEndSeq;
      aurora::gx::fifo::end_frame_async([] { HostAlloc host; sReleasedThisJob = false; n3dsGpuEndFrame(); if (!sReleasedThisJob) n3dsEndFrameRelease(); n3dsPaintHandedOff(); });
      while (sReleased.load(std::memory_order_acquire) < seq) svcSleepThread(50000);
    } else {
      aurora::gx::fifo::end_frame_async([] { HostAlloc host; n3dsGpuEndFrame(); n3dsPaintHandedOff(); });
      // Game-side dynamic arrays/textures can be reused by the next tick. The
      // worker must finish decoding and freeze its snapshot before that reuse;
      // the renderer owner still acknowledges early, retaining GPU overlap.
      aurora::gx::fifo::sync();
    }
    if (profWait) {
      static uint64_t waitTicks = 0, last = 0; static unsigned frames = 0;
      const uint64_t w1 = n3dsTick(); waitTicks += w1 - w0;
      if (++frames % 60 == 0) { if (last) n3dsLog("[game-wait] 60 frames: period %.2f ms, blocked at frame end %.2f ms, in fifo drain %.2f ms (%.1f calls), in fifo sync %.2f ms (%.1f calls) per frame\n", double(w1 - last) / 268111.856 / 60, double(waitTicks) / 268111.856 / 60, double(g_n3dsDrainTicks) / 268111.856 / 60, g_n3dsDrainCalls / 60.0, double(g_n3dsSyncTicks) / 268111.856 / 60, g_n3dsSyncCalls / 60.0); n3dsDrainCallersLog(); last = w1; waitTicks = 0; g_n3dsDrainTicks = g_n3dsSyncTicks = 0; g_n3dsDrainCalls = g_n3dsSyncCalls = 0; }
    }
    return;
  }
  aurora::gx::fifo::drain();
  aurora::gx::fifo::end_frame();
  { HostAlloc host; n3dsGpuEndFrame(); }
  n3dsPaintHandedOff();
}

void aurora_set_async_end_frame(bool) {}
bool aurora_get_async_end_frame() { return false; }
void aurora_frame_sync() { aurora::gx::fifo::sync(); n3dsGpuSync(); }
void aurora_fifo_profile_enable(bool) {}
void aurora_fifo_profile_get(uint64_t* out, size_t count) {
  for (size_t i = 0; i < count; i++) out[i] = 0;
}
void aurora_set_fifo_fast_paths(bool) {}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool) {}
void aurora_set_background_input(bool) {}
void aurora_set_resampler(AuroraSampler) {}
AuroraBackend aurora_get_backend() { return BACKEND_NULL; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count) *count = 0;
  return nullptr;
}
const AuroraStats* aurora_get_stats() { return &sStats; }
float aurora_get_fps() { return 0.f; }
void aurora_enable_vsync(bool) {}
void aurora_save_unbuilt_pipelines(void) {}

}  // extern "C"

// Modal settings owns the game thread. Freeze the game's OS clock as well, so
// timers/cutscenes don't jump forward after time spent in the panel.
extern "C" void n3dsSettingsClockPause(int paused){
  static float previous=1.0f;static bool active=false;
  if(paused&&!active){aurora::gx::fifo::sync();previous=aurora::time::scale();aurora::time::set_scale(0);active=true;}
  else if(!paused&&active){aurora::time::set_scale(previous);active=false;}
}
