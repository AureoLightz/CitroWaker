// One renderer/Citro3D owner, pinned to New 3DS application core 2.
#include <3ds.h>
#include <cstdlib>
#include <cstring>
#include "n3ds.h"
#include "performance_3ds.h"

void JKRPcBeginHostAlloc();
void JKRPcEndHostAlloc();
// The renderer expands full-width 4:3 draws horizontally to the 400x240 screen.
// Widen the game clipper by that same factor; wide_cull=0 selects the reference.
// Projection/depth are unchanged. Rebuild after performance header changes (corrected default).
extern "C" float n3dsCameraCullAspect(float aspect, float viewportWidth) {
  static const bool enabled = [] {
    Performance3ds config;
    if (FILE* f = std::fopen(N3DS_ROOT "/performance.ini", "r")) {
      config.read(f);
      std::fclose(f);
    }
    n3dsLog("[camera] wide_cull %d (full-width culling correction)\n", int(config.wideCull));
    return config.wideCull;
  }();
  return enabled && viewportWidth >= 639.5f ? aspect * 1.25f : aspect;
}

extern "C" { extern volatile unsigned g_n3dsMark[8]; extern uint64_t g_presentHeldTicks; }   // g_presentHeldTicks: gpu_3ds.cpp present pacing (not render work)
namespace {
Thread worker = nullptr;
LightLock submitLock;
LightEvent work, idle, acknowledged;
enum Operation { Init, Process, Frame, Fence, OwnerJob, Stop };
Operation operation;
const uint8_t* commandData;
uint32_t commandSize, commandUsed;
int commandDone, initResult;
bool released;
void (*ownerJob)(void*);
void* ownerContext;
uint32_t stagingBudget = 2u << 20;
// (Claude v120) paint pacing (gpu_3ds.cpp n3dsBeginLogicFrame): when the frame being rendered will be done, so the game paints only when
// the FIFO worker's hand-off will not stall it
volatile uint64_t frameStartTick = 0;   // 0 = no frame being rendered
volatile uint32_t frameEmaTicks = 0;
volatile uint32_t frameSlowTicks = 0, frameDevTicks = 0;   // slow average (1/16) and mean upward deviation: the paint cadence
// Most GX publications are short. Avoid sleeping/waking both cores for every
// tiny batch, but park after a bounded poll so long logic/render jobs don't spin.
void waitSoon(LightEvent* event) {
  for (unsigned i=0;i<8192;++i) if (LightEvent_TryWait(event)) return;
  LightEvent_Wait(event);
}
void workerMain(void*) {
  n3dsLog("[render-thread] owner requested CPU 2; processor query %d (Azahar 2126.2 stubs this SVC)\n", int(svcGetProcessorID()));
  for (;;) {
    waitSoon(&work);
    JKRPcBeginHostAlloc();
    released = false;
    const Operation op = operation;
    g_n3dsMark[5] = unsigned(op) + 1;
    if (op == Init) initResult = n3dsGpuInitLocal();
    else if (op == Process) commandUsed = n3dsGpuProcessLocal(commandData, commandSize, &commandDone);
    else if (op == Frame) {
      const uint64_t t0 = svcGetSystemTick(); frameStartTick = t0;
      n3dsGpuEndFrameLocal();
      {   // (Claude v120 DIAG) env N3DS_RENDER_DELAY_MS=a[-b]: the render core takes a..b ms longer per frame (console-like render times in Azahar)
        static int lo = -1, hi = 0; static uint32_t rng = 1;
        if (lo < 0) { const char* v = std::getenv("N3DS_RENDER_DELAY_MS"); lo = v ? std::atoi(v) : 0; const char* d = v ? std::strchr(v, '-') : nullptr; hi = d ? std::atoi(d + 1) : lo; }
        if (hi > 0) { rng = rng * 1103515245u + 12345u; const int ms = lo + int((rng >> 16) % uint32_t(hi - lo + 1)); svcSleepThread(int64_t(ms) * 1000000); }
      }
      const uint64_t el = svcGetSystemTick() - t0, held = g_presentHeldTicks; g_presentHeldTicks = 0;
      const uint32_t d = uint32_t(el > held ? el - held : 0);
      frameEmaTicks = frameEmaTicks ? uint32_t((uint64_t(frameEmaTicks) * 3 + d) / 4) : d;
      if (!frameSlowTicks) frameSlowTicks = d;
      else {
        frameSlowTicks = uint32_t((uint64_t(frameSlowTicks) * 15 + d) / 16);
        const uint32_t up = d > frameSlowTicks ? d - frameSlowTicks : 0;
        frameDevTicks = uint32_t((uint64_t(frameDevTicks) * 15 + up) / 16);
      }
      frameStartTick = 0;
    }
    else if (op == Stop) n3dsGpuShutdownLocal();
    else if (op == OwnerJob) ownerJob(ownerContext);
    g_n3dsMark[5] = 0;
    JKRPcEndHostAlloc();
    LightEvent_Signal(&idle);
    if (!released) LightEvent_Signal(&acknowledged);
    if (op == Stop) break;
  }
}
void submit(Operation op) {
  LightEvent_Wait(&idle);
  LightEvent_Clear(&idle);
  operation = op;
  LightEvent_Signal(&work);
  if (op == Process) waitSoon(&acknowledged);
  else LightEvent_Wait(&acknowledged);
}
}
extern "C" {
int n3dsRenderThreadStart(void) {
  if (worker) return 1;
  const char* on = std::getenv("N3DS_RENDER_THREAD");
  if (!on || std::strcmp(on,"1")) return 0;
  Performance3ds config;
  if (FILE* f = std::fopen(N3DS_ROOT "/performance.ini", "r")) {
    config.read(f);
    std::fclose(f);
  }
  // Keep speculation bounded; this opt-in only delays the existing fence.
  stagingBudget = (config.pipelineStage8 ? 8u : 2u) << 20;
  n3dsLog("[render-thread] bounded staging budget %lu KB\n", (unsigned long)(stagingBudget >> 10));
  bool isNew = false;
  APT_CheckNew3DS(&isNew);
  if (!isNew) { n3dsLog("[render-thread] New 3DS required; using inline renderer\n"); return 0; }
  LightLock_Init(&submitLock);
  LightEvent_Init(&work, RESET_ONESHOT);
  LightEvent_Init(&acknowledged, RESET_ONESHOT);
  LightEvent_Init(&idle, RESET_STICKY);
  LightEvent_Signal(&idle);
  worker = threadCreate(workerMain, nullptr, 512u << 10, 0x28, 2, false);
  if (!worker) { n3dsLog("[render-thread] core-2 threadCreate failed; using inline renderer\n"); return 0; }
  return 1;
}
int n3dsRenderThreadActive(void) { return worker != nullptr; }
void n3dsRenderThreadJob(void (*job)(void*), void* context) {
  LightLock_Lock(&submitLock);
  LightEvent_Wait(&idle); // prior owner must finish before its mailbox is replaced
  ownerJob = job; ownerContext = context;
  submit(OwnerJob);
  LightLock_Unlock(&submitLock);
}
uint32_t n3dsRenderStagingBudget(void) { return stagingBudget; }
// (Claude v120) the render core's time per frame: slow average + mean upward deviation (0 = no render thread / no frame yet)
uint64_t n3dsRenderFrameTicks(void) { return worker ? uint64_t(frameSlowTicks) + frameDevTicks : 0; }
// (Claude v120) ticks until the render core finishes the frame it is rendering (estimate; 0 = idle or no render thread)
uint64_t n3dsRenderRemainingTicks(uint64_t now) {
  const uint64_t s = frameStartTick;
  if (!worker || !s) return 0;
  const uint64_t end = s + frameEmaTicks;
  return end > now ? end - now : 1;   // overdue: still busy, about to finish
}
int n3dsRenderThreadInit(void) {
  LightLock_Lock(&submitLock); submit(Init); LightLock_Unlock(&submitLock); return initResult;
}
uint32_t n3dsRenderThreadProcess(const uint8_t* data, uint32_t size, int* done) {
  LightLock_Lock(&submitLock);
  // Wait BEFORE overwriting the previous job's mailbox.
  LightEvent_Wait(&idle);
  commandData = data; commandSize = size;
  submit(Process);
  const uint32_t used = commandUsed; *done = commandDone;
  LightLock_Unlock(&submitLock);
  return used;
}
void n3dsRenderThreadFrame(void) {
  LightLock_Lock(&submitLock); submit(Frame); LightLock_Unlock(&submitLock);
}
void n3dsRenderThreadRelease(void) {
  // Called on owner only, after snapshots are complete. No caller accesses
  // the mailbox again until idle, even when the producer is acknowledged early.
  released = true;
  LightEvent_Signal(&acknowledged);
}
// (Claude v72) Host allocation failed on another thread: wait (at most 1 s) until the render thread is idle, so the recorded frame it
// rendered has been released, and ask for one retry. Never waits on the render thread itself.
extern "C" int n3dsHostAllocRetry(size_t size) {
  if (!worker || threadGetCurrent() == worker) return 0;
  static unsigned reported = 0;
  const int busy = !LightEvent_TryWait(&idle);
  const int timedOut = busy ? LightEvent_WaitTimeout(&idle, 1000000000LL) : 0;
  if (reported++ < 16) n3dsLog("[mem] host allocation of %u bytes failed; render thread %s, retrying\n", unsigned(size), !busy ? "already idle" : timedOut ? "still busy after 1 s" : "now idle");
  return 1;
}
int n3dsRenderThreadBusy(void) { return worker && !LightEvent_TryWait(&idle); }   // idle is RESET_STICKY: TryWait does not clear it
void n3dsGpuSync(void) {
  if (!worker) return;
  LightLock_Lock(&submitLock); submit(Fence); LightLock_Unlock(&submitLock);
}
void n3dsGpuShutdown(void) {
  if (!worker) { n3dsGpuShutdownLocal(); return; }
  LightLock_Lock(&submitLock); submit(Stop); LightLock_Unlock(&submitLock);
  threadJoin(worker, U64_MAX); threadFree(worker); worker = nullptr;
}
}
