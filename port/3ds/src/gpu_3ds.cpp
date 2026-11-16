extern "C" { unsigned g_n3dsModelDirect=0; unsigned g_n3dsMaterialIdentity=0; unsigned g_n3dsModelRecords=0; unsigned g_n3dsMaterialRecords=0; }
#include "paint_budget_3ds.h"
#include <cstdint>
#include <cstdlib>
namespace t2p { extern uint64_t (*g_compileClock)(); extern uint64_t g_compilePhase[8]; }
extern void (*g_r3dsTrace)(const char*, int);   // (Claude) hw6 renderer progress points (texcache.cpp), set for the watchdog
extern int g_hwTest;   // (Claude) hw6 backend3ds.cpp console bisection variant
extern bool g_shaderNoDph;   // hw6 vtx.cpp
extern int g_shaderOutTest;   // hw6 vtx.cpp
#include <malloc.h>
#include <atomic>
static uint64_t g_recStateProf[8];   // (Claude v100) copied from the recorder thread's g_t2pStateProf at each end of frame
static std::atomic<int> g_foliageAb{-1};   // (Claude v100) N3DS_AB_CYCLE overrides foliage_far per phase; -1: the setting
static std::atomic<int> g_abDrawReuse{-1};   // (Claude v97) N3DS_AB_CYCLE: wanted draw-reuse state, -1 = not cycling
extern "C" unsigned n3dsFifoDrainStalls;   // (Claude v96) gen/fifo_3ds.cpp: game-thread waits instead of FIFO buffer growth past 1 MiB
// 3DS side of the port (built with devkitARM g++, like the renderer): the screens, citro3d, the GX renderer
// (Renderer3ds), HID input and the log. The game side (clang, Aurora's GX layer) reaches it through n3ds.h only, so
// no libctru / citro3d struct crosses the compiler boundary (devkitARM uses short enums, the clang build does not).
#include <3ds.h>
#include <citro3d.h>

#include <dirent.h>
#include <sys/iosupport.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "n3ds.h"
#include "renderer3ds.h"
#include "packet_worker3ds.h"
#include "texcache.h"
#include "texconv.h"
#include "render_snapshot_3ds.h"
#include "record_frame_3ds.h"
#include "material_program_pool.h"
#include "prepared_material_store.h"
static t2p::MaterialProgramPool sMaterialPrograms;
static t2p::MaterialVersions sMaterialVersions;
static t2p::PreparedMaterialStore sPreparedMaterials;
static bool sPreparedMaterialsEnabled=false;
extern "C" volatile unsigned g_n3dsShapeGen;
extern "C" void n3dsMaterialWriting(const void* p,uint32_t n){sMaterialVersions.writing(p,n);}
extern "C" uint64_t n3dsMaterialRegisterVersioned(const uint8_t* p,uint32_t n){
 static const bool audit=[]{const char* a=std::getenv("N3DS_MODEL_ID_AUDIT");return a&&*a=='1';}();
 const auto version=sMaterialVersions.sample(p,n,g_n3dsShapeGen);
 if(sPreparedMaterialsEnabled)return sMaterialPrograms.publishPrepared(p,n,version,audit,[](const uint8_t* bytes,size_t size,const std::shared_ptr<const t2p::PreparedMaterial>& previous,std::vector<uint32_t>& args){return sPreparedMaterials.resolve(bytes,size,previous,args);});
 return sMaterialPrograms.publishVersioned(p,n,version,audit);
}

extern "C" uint64_t n3dsMaterialRegister(const uint8_t* p,uint32_t n) { return sMaterialPrograms.publish(p,n); }
extern "C" const t2p::GxStateProgram* n3dsMaterialBorrow(uint64_t t) { return sMaterialPrograms.borrowed(t); }
extern "C" void n3dsMaterialRelease(uint64_t t) { sMaterialPrograms.release(t); }

namespace t2p { extern uint64_t g_replayProf[16]; extern bool g_replayProfOn; }   // hw6 renderer3ds.cpp (v119 diag)
#include "performance_3ds.h"
#include "bottom_ui_3ds.h"
#include "depth_visibility_3ds.h"
#include "depth_probe_3ds.h"
#include "core_probe_3ds.h"
extern "C" uint64_t g_n3dsDecodeCount[2];   // (Claude v95 diag) renderer3ds.cpp
extern "C" volatile unsigned g_n3dsShapeGen;   // (Claude v95) JKRHeap.cpp: model-memory generation
#include "uniform_storage.h"
#include <unistd.h>
// (Claude) heap_fence: room for more core-2 staging while the host heap has room: in use <= 85 MiB and
// >= 3 MiB still left for sbrk (taken + sbrk room is ~93 MB in Azahar; the old "OOM at ~88.9 MB" was only
// the last [mem] reading before sbrk ran out). mallinfo takes newlib's malloc lock; called once per 512 KB
// of estimated staging past the budget.
extern "C" char* fake_heap_end;
extern "C" { extern volatile int g_n3dsDlCensusOpen; void n3dsDlCensusWrite(const char*); }   // origin_prof_3ds.cpp (Claude v92)
extern "C" void n3dsLockStatsLog(unsigned long frame); extern "C" void n3dsAllocSitesLog(unsigned long frame);   // main_3ds.cpp (Claude v74)
void n3dsFastAllocEnable(bool on, int maxSpans = 0); void n3dsFastAllocLog();   // main_3ds.cpp (Claude v76)   // libctru: the end of the heap sbrk extends into
static size_t brkRoom() { char* brk=(char*)sbrk(0); return brk && brk!=(char*)-1 && brk<fake_heap_end ? size_t(fake_heap_end-brk) : 0; }
static uint64_t g_originTicks[2], g_originBytes[2], g_originDraws[2], g_originVerts[2], g_originCalls[2];
static size_t g_minBrkRoom = ~size_t(0);   // lowest sbrk room seen by heapRoom since the last [pipeline] log
static size_t g_heapCeiling = 85u << 20;
   // (Claude) N3DS_HEAP_TEST_CEILING_MB lowers it in emulator tests to force the fence paths
static std::atomic<bool> g_fenceRoomOnly{false};   // (Claude v98) the v97 fence (needs 3 MiB of sbrk room); N3DS_AB_CYCLE alternates it
extern "C" void n3dsPoolsTrim(void) { t2p::UniformBlockPool::get().trim(); t2p::EventNodePool::get().trim();
#if defined(N3DS_EVENT_INDIRECT) && N3DS_EVENT_INDIRECT
 t2p::DirectLegacyStatePool::get().trim();
#endif
 }   // (Claude v86) main_3ds.cpp malloc retry
// (Claude v102) performance.ini pool_floor (default 512 events, a quarter as many uniform blocks; 0 = v86 behaviour: empty the pools)
namespace { void poolsFenceTrim(); }   // (Claude v102) defined after g_perf (same anonymous namespace)
static bool heapRoom(size_t* inUse) {
  const struct mallinfo mi = mallinfo();
  const size_t room = brkRoom();
  *inUse = size_t(mi.uordblks);
  g_minBrkRoom = std::min(g_minBrkRoom, room);
  // (Claude v98) free blocks inside the heap count too: once the heap has reached its peak (taken ~91 of 93 MB), sbrk room never returns above
  // ~2 MB, so "room >= 3 MiB" fenced every frame (v97 console: decode waited 20-80 ms/frame with ~9 MB free inside the heap). Large requests that
  // no free block holds fall back to the out-of-memory retry (pool trim; operator new waits for the render thread).
  const bool ok = size_t(mi.uordblks) <= g_heapCeiling && (g_fenceRoomOnly ? room >= (3u << 20) : room + size_t(mi.fordblks) >= (6u << 20));
  if (!ok) poolsFenceTrim();   // (Claude v86) short of heap: kept pool blocks go back to newlib (v102: down to a floor)
  return ok;
}
// (Claude v67) staging_partial: enough freed inside the heap. 5 MiB usable (free blocks + sbrk room) and 1.5 MiB of sbrk room kept for blocks the
// free lists cannot hold; the recorder re-asks every 512 KiB of estimate (estimate >= real growth).
static bool heapReady() {
  const struct mallinfo mi = mallinfo();
  const size_t room = brkRoom();
  g_minBrkRoom = std::min(g_minBrkRoom, room);
  const bool ok = size_t(mi.uordblks) <= g_heapCeiling && (g_fenceRoomOnly ? room >= (3u << 20) : true) && size_t(mi.fordblks) + room >= (8u << 20);
  if (!ok) poolsFenceTrim();   // (Claude v86, v102 floor)
  return ok;   // (Claude v72) 1.5->3 MiB room, 5->8 MiB usable: v71 ran out at a stage load (Orca's house)
}


// Citro3D internal queue fence: used only for opt-in presentation measurements
// and teardown. It waits for transfers/swap requests without submitting a frame.
extern "C" void C3Di_RenderQueueWaitDone(void);
extern "C" uint64_t g_n3dsIndirectDraws, g_n3ds2dDraws, g_n3ds2dHash;   // (Claude v106 diag) renderer3ds.cpp

namespace {

constexpr int TW = 240, TH = 400;

// Guest memory is host memory: Aurora's stream carries host pointers (32-bit on the 3DS).
struct HostMem : t2p::GuestMem {
  RenderSnapshot3ds snapshot;
  const RenderSnapshot3ds* view=nullptr;
  bool frozen = false, verifying = true;
  mutable uint32_t missing = 0, verifyMissing = 0;
  const uint8_t* read(uint32_t addr, size_t size) const override {
    if (!frozen) return addr ? reinterpret_cast<const uint8_t*>(uintptr_t(addr)) : nullptr;
    const RenderSnapshot3ds& sn = view ? *view : snapshot;
    const auto* p = sn.read(addr,size);
    if (!p && addr && sn.isKnown(addr,size)) return reinterpret_cast<const uint8_t*>(uintptr_t(addr));   // (Claude v102) skipped (cache held it)
    if (!p) { if (verifying) ++verifyMissing; else ++missing; }
    return p; // Never fall back to live guest reads while the producer runs (except ranges the cache already held, above).
  }
  bool hashHint(uint32_t addr, size_t size, uint64_t& hash) const override {   // (Claude v102)
    if (!frozen) return false;
    return (view ? *view : snapshot).hint(addr, size, hash);
  }
};

HostMem g_mem;
extern "C" void n3dsAudioHealth(void);
Performance3ds g_perf;
void poolsFenceTrim() {   // (Claude v102) performance.ini pool_floor (default 512 events, a quarter as many uniform blocks; 0 = empty them, v86-v101)
  const unsigned f = unsigned(g_perf.poolFloor);
  t2p::UniformBlockPool::get().trimTo(f / 4); t2p::EventNodePool::get().trimTo(f);
#if defined(N3DS_EVENT_INDIRECT) && N3DS_EVENT_INDIRECT
  t2p::DirectLegacyStatePool::get().trimTo(f/4);
#endif
}
bool g_pipeline=false;
t2p::CpuFrame3ds* g_cpuFrames[2]={};
t2p::FrameRecorder3ds* g_recorder=nullptr;
t2p::CpuFrame3ds* g_submitted=nullptr;
unsigned g_recordIndex=0;
t2p::Renderer3ds* g_r = nullptr;
C3D_RenderTarget* g_tgt = nullptr;
DepthVisibility3ds g_depthVisibility;
FILE* g_log = nullptr;
LightLock g_logLock;
bool g_consoleOn = true;
BottomUi3ds g_bottomUi;
std::atomic<bool> g_routeOwnsBottom{false};   // (Claude v80) the map route's bottom target is linked to GFX_BOTTOM
std::atomic<uint32_t> g_frames{0};
// (Claude) Console diagnosis markers, printed by the watchdog: [0] fifo drains entered, [1] drains left (game thread), [2] worker process calls
// entered, [3] left, [4] end-of-frame stage on the worker (1 entered, 2 recorded, 3 after sync with the renderer, 4 after hand-off, 0 out),
// [5] renderer operation in progress (operation + 1, 0 idle), [6] mailbox-poll yields, [7] renderer end-of-frame stage.
extern "C" { volatile unsigned g_n3dsMark[8] = {}; }
static const char* volatile g_traceWhat = "-";
static volatile int g_traceIdx = 0;
static volatile unsigned g_traceCount = 0;
std::atomic<uint32_t> g_inputFrames{0};
size_t g_missesWritten = 0;   // misses-bundle entries already on the SD card
static bool g_noBundleWrites = false;   // (Claude, release prep) N3DS_NO_BUNDLES
// (Claude v120) paint pacing state shared by the game thread, the FIFO worker and the render core (see n3dsBeginLogicFrame)
static std::atomic<uint32_t> g_paintEndedN{0}, g_paintHandedN{0};
static uint64_t g_paintSlotRing[8] = {};   // game thread writes (painted tick's 30 Hz slot), indexed by painted-frame number
static uint64_t g_submittedSlot = 0;       // FIFO worker: the slot of the frame handed to the render core
static uint64_t g_renderSlot = 0;          // render core: the slot of the frame it renders
static uint64_t n3dsPaintSlotOf(uint32_t n) { return g_paintSlotRing[n & 7]; }
static uint32_t g_paintHandedCount() { return g_paintHandedN.load(std::memory_order_acquire); }
uint64_t g_processTicks = 0;
std::vector<uint8_t> g_xfb;
std::string g_errLast;
std::vector<uint64_t> g_shotFrames;   // N3DS_ROOT/shots.txt: frame numbers whose picture is saved to shots.bin (name[32] + 240x400 RGBA, as replay3ds)
FILE* g_shots = nullptr;
uint64_t g_pendingShot = 0;
// (Claude v82) console capture: SELECT alone (it does nothing in the game) saves the next rendered top frame, colour AND depth, to
// N3DS_ROOT/captures.bin (name[32] + 240x400 RGBA8 colour + 240x400 D24S8 depth, both in the GPU's tiled layout), so hardware-only picture bugs
// can be compared with the emulator. Nothing is written unless SELECT is pressed.
std::atomic<bool> g_captureRequest{false};
uint64_t g_pendingCapture = 0;
unsigned g_captureCount = 0;
uint32_t g_errCount = 0;

// Observe texture bindings only. Recording, merging, and decoding still belong
// entirely to Renderer3ds, on the same owner thread as this forwarding sink.
struct TextureRanges : t2p::GxSink {
  uint32_t primitiveRunLimit() const override {return g_r->primitiveRunLimit();}
  void collect(const t2p::GxRegs& r) {
    if (!n3dsRenderThreadActive()) return;
    for (int m=0;m<8;++m) {
      const int off=(m<4 ? m : m-4)+(m<4 ? 0 : 0x20);
      if (!r.bpWritten[0x88+off]) continue;
      const uint32_t image=r.bp[0x88+off]&0xffffff, mode=r.bp[0x80+off]&0xffffff;
      const uint32_t addr=r.texAddr[m] ? r.texAddr[m] : (r.bp[0x94+off]&0x1fffff)<<5;
      if (!addr || g_r->cache().renderedAt(addr)) continue;
      t2p::GxTexBinding binding;
      binding.width=int(image&1023)+1; binding.height=int((image>>10)&1023)+1;
      binding.fmt=int((image>>20)&15); binding.mipMode=uint8_t((mode>>5)&3);
      binding.maxLod=float((r.bp[0x84+off]>>8)&255)/16.0f;
      size_t bytes=0; int w=binding.width,h=binding.height;
      for (int level=0;level<binding.mipLevels();++level) {
        bytes+=t2p::gxTextureBytes(binding.fmt,w,h);
        w=std::max(1,w/2); h=std::max(1,h/2);
      }
      g_mem.snapshot.add(addr,bytes<=UINT32_MAX ? uint32_t(bytes) : 0);
    }
  }
  void onDraw(const t2p::GxRegs& r,const t2p::GxDraw& d) override { collect(r); g_r->onDraw(r,d); }
  t2p::GxDraw* appendTarget(const t2p::GxRegs& r,const t2p::GxDraw& h) override { collect(r); return g_r->appendTarget(r,h); }
  void onAppended(const t2p::GxRegs& r,t2p::GxDraw& d,uint32_t first) override { g_r->onAppended(r,d,first); }
  void onCopy(const t2p::GxRegs& r) override { g_r->onCopy(r); }
  void onTlutLoad(const t2p::GxRegs& r) override { g_r->onTlutLoad(r); }
  void onError(const std::string& e) override { g_r->onError(e); }
  const uint8_t* resolveDisplayList(uint32_t a,uint32_t n) override { return g_r->resolveDisplayList(a,n); }
} g_ranges;
uint32_t g_snapshotFallbacks=0;
uint64_t g_lastPresent=0;
double g_presentSum=0,g_presentMin=1e30,g_presentMax=0;
unsigned g_presentCount=0;
bool g_measurePresent=false;
// N3DS_FRAME_LOG=a-b (measurement only, default off): one sample per frame a..b kept in this fixed
// array (no allocation, no SD writes inside the window), written as "[frame]" lines after frame b.
// Records only values the frame already measures (+ texture-cache counter deltas); per-draw
// build/plan/draw timing stays as before (every 60th frame) unless N3DS_FRAME_LOG_DRAWS=1,
// because it costs four kernel calls per draw.
struct FrameSample { uint32_t frame, drawn, gx, texMiss, texConv, texEvict, texKB; float process, render, build, plan, draw, present; u64 presentTick; };
constexpr uint32_t kFrameLogMax = 3000;   // (Claude v94) 600 -> 3000: cutscene showcase timing (192 KB static)
FrameSample g_frameLog[kFrameLogMax];
uint32_t g_frameLogFrom = 0, g_frameLogTo = 0, g_frameLogCount = 0;
bool g_frameLogDraws = false;
t2p::TexCacheStats g_frameLogTex;
bool g_textureDiag=false;

// (Claude v102) snapshot skip: the renderer's cache already holds this texture with the same sample hash (performance.ini snapshot_skip, default 1)
static bool snapshotSkip(uint32_t addr, uint32_t size, const uint8_t* live, uint64_t& hash) {
  if (!g_perf.snapshotSkip || !g_r) return false;
  // (Claude v116) EFB copy destinations: the renderer samples its GPU copy, never these bytes (hash 0: no converted-entry hint)
  if (g_perf.snapshotCopies && t2p::texRenderedAt(addr)) { hash = 0; return true; }
  hash = t2p::sampleHashOf(live, size, g_r->cache().sampleBytes());
  return t2p::texKnownContains(addr, size, hash);
}
extern "C" char* fake_heap_start; extern "C" char* fake_heap_end;   // newlib: the application heap (main_3ds.cpp sets them)
const uint8_t* readableTexture(uint32_t addr,uint32_t size) {
  uint64_t p=addr,end=uint64_t(addr)+size;
  // (Claude v102) inside the application heap: readable, no svcQueryMemory per texture (was one kernel call per range per frame)
  if (addr && uintptr_t(fake_heap_start) <= addr && end <= uint64_t(uintptr_t(fake_heap_end))) return reinterpret_cast<const uint8_t*>(uintptr_t(addr));
  while (p<end) {
    MemInfo info; PageInfo page;
    if (R_FAILED(svcQueryMemory(&info,&page,uint32_t(p))) || !(info.perm&MEMPERM_READ) ||
        uint64_t(info.base_addr)+info.size<=p) return nullptr;
    p=uint64_t(info.base_addr)+info.size;
  }
  return reinterpret_cast<const uint8_t*>(uintptr_t(addr));
}

// (Claude v115) asynchronous log: v114 wrote + fflush'ed every line on the SD card under one lock shared by the game, decode and render
// threads; the ~50 profile lines every 60 frames stalled the game ~300 ms (one hitch per window). Lines now go to a memory buffer that a
// low-priority writer thread flushes every 500 ms. Exit / watchdog / hang lines (and everything before the writer starts) flush at once.
// (Claude v120) the pending lines live in two static buffers, never in operator new memory: v115's std::string took its blocks from
// whatever JKR heap the game had current, and the writer thread freed them through JKRHeap::free (not thread-safe, and the heap may
// already be gone: the game frees heaps wholesale when it loads a save) - Luma data abort in JKRExpHeap::do_free on opening a save.
// Game threads append to g_logBuf[g_logCur] under g_logLock; the flusher (g_logFileLock held) swaps the index and writes the other one.
constexpr size_t kLogBufBytes = 192u << 10;
char g_logBuf[2][kLogBufBytes]; size_t g_logLen[2] = {0, 0}; int g_logCur = 0;
size_t g_logDropped = 0; LightLock g_logFileLock; bool g_logAsync = false;
void logFlushPendingLocked() {   // g_logFileLock held; takes g_logLock briefly
  LightLock_Lock(&g_logLock);
  const int idx = g_logCur; g_logCur ^= 1; g_logLen[g_logCur] = 0;
  const size_t n = g_logLen[idx]; const size_t dropped = g_logDropped; g_logDropped = 0; FILE* f = g_log;
  LightLock_Unlock(&g_logLock);
  if (!f) return;
  if (n) std::fwrite(g_logBuf[idx], 1, n, f);
  if (dropped) std::fprintf(f, "[log] %u bytes of log lines dropped (buffer full)\n", unsigned(dropped));
  if (n || dropped) std::fflush(f);
}
void logWriterThread(void*) {
  for (;;) {
    svcSleepThread(500000000LL);
    LightLock_Lock(&g_logFileLock); logFlushPendingLocked(); LightLock_Unlock(&g_logFileLock);
  }
}
bool logUrgent(const char* p, size_t n) {
  static const char* const kUrgent[] = {"[exit]", "[watchdog]", "[hang", "[crash", "[fatal", "[abort", "[hwtest]", "[oom"};
  for (const char* u : kUrgent) { const size_t k = std::strlen(u); if (n >= k && !std::memcmp(p, u, k)) return true; }
  return false;
}
void logWrite(const char* p, size_t n) {
  if (g_logAsync && !logUrgent(p, n)) {
    LightLock_Lock(&g_logLock);
    size_t& len = g_logLen[g_logCur];
    if (len + n <= kLogBufBytes) { std::memcpy(g_logBuf[g_logCur] + len, p, n); len += n; } else g_logDropped += n;
    svcOutputDebugString(p, int(n));
    if (g_consoleOn) std::fwrite(p, 1, n, stdout);
    LightLock_Unlock(&g_logLock);
    return;
  }
  LightLock_Lock(&g_logFileLock);
  logFlushPendingLocked();
  LightLock_Lock(&g_logLock);
  if (g_log) { std::fwrite(p, 1, n, g_log); std::fflush(g_log); }
  svcOutputDebugString(p, int(n));
  if (g_consoleOn) std::fwrite(p, 1, n, stdout);
  LightLock_Unlock(&g_logLock);
  LightLock_Unlock(&g_logFileLock);
}

ssize_t errWrite(struct _reent*, void*, const char* p, size_t n) {
  logWrite(p, n);
  return ssize_t(n);
}
const devoptab_t kErrDev = {"n3dserr", 0, nullptr, nullptr, errWrite, nullptr};

void push32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

}  // namespace

#include "settings_menu_3ds.h"

extern "C" {

void n3dsLog(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) logWrite(buf, size_t(n) < sizeof buf ? size_t(n) : sizeof buf - 1);
}

static void n3dsPgoTick(unsigned frame);   // (Claude v111) below
static const char* g_logPath=N3DS_ROOT "/log.txt";
void n3dsSetLogPath(const char* path){if(path)g_logPath=path;}

static std::atomic<bool> g_processExiting{false};
static u32 g_mainThreadId=0;
int n3dsPrepareExit(void){
  g_processExiting.store(true);
  if(g_mainThreadId){LightLock_Lock(&g_logLock);g_consoleOn=false;LightLock_Unlock(&g_logLock);}
  u32 id=0;svcGetThreadId(&id,CUR_THREAD_HANDLE);
  return g_mainThreadId && id==g_mainThreadId;
}
void n3dsCloseLog(void){LightLock_Lock(&g_logFileLock);g_logAsync=false;logFlushPendingLocked();LightLock_Lock(&g_logLock);if(g_log){std::fclose(g_log);g_log=nullptr;}LightLock_Unlock(&g_logLock);LightLock_Unlock(&g_logFileLock);}
extern "C" void n3dsFlushLog(void){LightLock_Lock(&g_logFileLock);logFlushPendingLocked();LightLock_Unlock(&g_logFileLock);}
int n3dsInit(void) {
  LightLock_Init(&g_logLock);
  LightLock_Init(&g_logFileLock);
  svcGetThreadId(&g_mainThreadId,CUR_THREAD_HANDLE);
  osSetSpeedupEnable(true);
  gfxInitDefault();
  gfxSetWide(false);
  consoleInit(GFX_BOTTOM, nullptr);
  mkdir("sdmc:/3ds", 0777);
  mkdir(N3DS_ROOT, 0777);
  // (Claude) console GPU-hang bisection: 3ds/ww3ds/hwtest.txt holds the number of the next variant; each launch takes it, stores the next one and
  // logs to its own file (log_test<N>.txt), so several launches in a row answer several questions. Absent: normal run, log.txt.
  int hwTest = -1;
  // A leftover test counter must never activate destructive/rebooting diagnostics.
  bool hwTestRequested = false;
  if (FILE* enable = std::fopen(N3DS_ROOT "/hwtest.enable", "r")) { hwTestRequested = true; std::fclose(enable); }
  if (hwTestRequested)
  if (FILE* tf = std::fopen(N3DS_ROOT "/hwtest.txt", "r")) { if (std::fscanf(tf, "%d", &hwTest) != 1) hwTest = -1; std::fclose(tf); }
  if (osGetMemRegionSize(MEMREGION_APPLICATION) < (90u << 20)) hwTest = -1;   // the 64 MB start that only reboots into the title: not a test launch
  if (hwTest >= 0) {
    if (FILE* tf = std::fopen(N3DS_ROOT "/hwtest.txt", "w")) { std::fprintf(tf, "%d\n", hwTest + 1); std::fclose(tf); }
    char name[64]; std::snprintf(name, sizeof name, N3DS_ROOT "/log_test%d.txt", hwTest);
    g_log = std::fopen(name, "w");
    g_hwTest = hwTest <= 5 ? hwTest : 0;
    g_shaderOutTest = g_hwTest >= 1 && g_hwTest <= 4 ? g_hwTest : 0;
    // round 3 variants live in hw6 backend3ds.cpp (g_hwTest 1..6); performance.ini is left to the defaults
    std::remove(N3DS_ROOT "/performance.ini");
  } else
  g_log = std::fopen(g_logPath, "w");
  devoptab_list[STD_ERR] = &kErrDev;   // write(2, ...) and stderr: the log file, the debug output and the bottom screen
  if (hwTest < 0 && !std::getenv("N3DS_LOG_SYNC")) {   // (Claude v115) async log writer (env N3DS_LOG_SYNC=1 = every line flushed at once)
    Thread lt = threadCreate(logWriterThread, nullptr, 0x2000, 0x3F, 1, true);
    if (!lt) lt = threadCreate(logWriterThread, nullptr, 0x2000, 0x3F, 0, true);
    g_logAsync = lt != nullptr;
  }
  setvbuf(stderr, nullptr, _IONBF, 0);
  bool isNew = false;
  APT_CheckNew3DS(&isNew);
  extern u32 __ctru_heap_size, __ctru_linear_heap_size;
  if (hwTest >= 0) n3dsLog("[hwtest] launch %d, variant %d\n", hwTest, g_hwTest);
  n3dsLog("[3ds] Wind Waker 3DS port: New 3DS %d; application memory %lu KB (free %lu KB), heap %lu KB, linear heap %lu KB\n", int(isNew),
          (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10), (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10),
          (unsigned long)(__ctru_heap_size >> 10), (unsigned long)(__ctru_linear_heap_size >> 10));
  // (Claude) Console diagnosis: for the first two minutes a thread on core 2 logs every 2 s how far the game got, so a freeze on real hardware
  // shows where it stopped (frames still counting = slow, not frozen; audio blocks still counting = audio threads alive).
  g_r3dsTrace = [](const char* what, int index) { g_traceWhat = what; g_traceIdx = index; g_traceCount = g_traceCount + 1; };
  // the bisection goes on by itself: reboot into this title (the next launch takes the next variant)
  static const auto hwTestNext = [] {
    u64 tid = 0;
    if (R_FAILED(APT_GetProgramID(&tid)) || R_FAILED(nsInit())) { n3dsLog("[hwtest] cannot reboot by itself\n"); return; }
    if (g_log) { std::fflush(g_log); std::fclose(g_log); g_log = nullptr; }
    NS_RebootToTitle(MEDIATYPE_SD, tid, 6);
    for (;;) svcSleepThread(100000000LL);
  };
  // starvation probes on core 0: one at the GSP event thread's priority, one at the lowest; each counts 10 ms sleeps
  static volatile unsigned probeHigh = 0, probeLow = 0;
#if defined(N3DS_SCHED_PROBES) && N3DS_SCHED_PROBES
  threadCreate([](void*) { for (;;) { svcSleepThread(10000000LL); probeHigh = probeHigh + 1; } }, nullptr, 0x1000, 0x1A, 0, true);
  threadCreate([](void*) { for (;;) { svcSleepThread(10000000LL); probeLow = probeLow + 1; } }, nullptr, 0x1000, 0x3F, 0, true);
#else
  n3dsLog("[watchdog] scheduler probes disabled; two diagnostic thread slots freed\n");
#endif
  static volatile unsigned* const probes[2] = {&probeHigh, &probeLow};
  static Thread watchdog = threadCreate([](void*) {
    extern unsigned g_n3dsAudioBlocks;
    extern void* g_n3dsGxQueue; extern unsigned* g_n3dsCmdListAddr[4]; extern unsigned g_n3dsCmdListSize[4]; extern unsigned g_n3dsCmdListCount;
    unsigned long seen[2] = {~0ul, ~0ul}; bool dumped = false;
    for (int i = 1; i <= 60; i++) {
      svcSleepThread(2000000000LL);
      if(g_processExiting.load())return;
      const unsigned long now = g_frames.load();
      static const bool dumpTest = std::getenv("N3DS_HANG_DUMP_TEST") != nullptr;   // emulator check of this path
      // (Claude v84) not while the HOME menu has the game suspended (v83 console log: a 10 s HOME visit was dumped as a GPU hang)
      if (!aptIsActive()) { seen[0] = seen[1] = ~0ul; continue; }
      if (!dumped && ((now > 0 && now == seen[0] && now == seen[1]) || (dumpTest && i == 1))) {   // no frame for 4 s: what the GPU was given
        dumped = true;
        if (const gxCmdQueue_s* q = static_cast<const gxCmdQueue_s*>(g_n3dsGxQueue)) {
          n3dsLog("[gpu-hang] GX queue: %u entries, next to submit %u, completed %u (capacity %u)\n", q->numEntries, q->curEntry, q->lastEntry, q->maxEntries);
          for (unsigned e = 0; e < q->numEntries && e < 16; e++) {
            const u32* d = q->entries[e].data;
            n3dsLog("[gpu-hang]   entry %u: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n", e, (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2], (unsigned long)d[3], (unsigned long)d[4], (unsigned long)d[5], (unsigned long)d[6], (unsigned long)d[7]);
          }
        } else n3dsLog("[gpu-hang] no GX queue bound\n");
        for (unsigned k = 0; k < 1 && k < g_n3dsCmdListCount; k++) {
          const unsigned slot = (g_n3dsCmdListCount - 1 - k) & 3;
          const unsigned* a = g_n3dsCmdListAddr[slot]; const unsigned sz = g_n3dsCmdListSize[slot];
          n3dsLog("[gpu-hang] command list -%u: address %p, %u bytes (list number %u)\n", k, (const void*)a, sz, g_n3dsCmdListCount - k);
          char name[96]; std::snprintf(name, sizeof name, N3DS_ROOT "/hang_v%d_cmdlist_%u.bin", g_hwTest, k);
          if (a && sz && sz <= (2u << 20)) if (FILE* f = std::fopen(name, "wb")) { std::fwrite(a, 1, sz, f); std::fclose(f); }
        }
        n3dsLog("[gpu-hang] saved\n");
        if (g_hwTest) { n3dsLog("[hwtest] variant %d HANGS at frame %lu; rebooting into the next one\n", g_hwTest, now); hwTestNext(); }
      }
      if (g_hwTest && i == 8 && now > 60) { n3dsLog("[hwtest] variant %d RUNS (frame %lu after 16 s); rebooting into the next one\n", g_hwTest, now); hwTestNext(); }
      seen[1] = seen[0]; seen[0] = now;
      n3dsLog("[watchdog] %d s: frame %lu, audio blocks %u, drains %u/%u, worker process %u/%u end-frame stage %u, renderer op %u stage %u, mail yields %u; renderer at \"%s\" %d (step %u); core-0 probes prio 0x1A %u, 0x3F %u\n", i * 2, (unsigned long)g_frames.load(), g_n3dsAudioBlocks, g_n3dsMark[0], g_n3dsMark[1], g_n3dsMark[2], g_n3dsMark[3], g_n3dsMark[4], g_n3dsMark[5], g_n3dsMark[7], g_n3dsMark[6], g_traceWhat, g_traceIdx, g_traceCount, *probes[0], *probes[1]);
    }
  }, nullptr, 16u << 10, 0x19, isNew ? 2 : 0, true);
  (void)watchdog;
  return isNew ? 1 : 0;
}


// (Claude v101) [cache-test]: does data the CPU wrote and flushed (GSPGPU_FlushDataCache, what C3D_TexFlush does) reach RAM, where the GPU
// reads it? The GPU's DMA engine (TextureCopy) reads the source from RAM; the CPU then reads the copy with its cache dropped. Variants:
// written on this core, written on another core (the renderer's textures are flushed by whichever thread converts them), and rewritten
// after an earlier flush. Console-only garbage texels (horizon clouds, menu corners) while the CPU sees correct bytes would show here as
// mismatched 32-byte lines. If any mismatch with the New 3DS L2 cache on, the test runs again with it off and keeps it off if that is clean.
namespace cachetest {
static void fill(u8* p, size_t n, u32 seed) { u32 x = seed * 2654435761u + 1; for (size_t i = 0; i < n; i += 4) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; std::memcpy(p + i, &x, 4); } }
struct Job { u8* p; size_t n; u32 seed; };
static void fillThread(void* a) { Job* j = static_cast<Job*>(a); fill(j->p, j->n, j->seed); }
static unsigned compare(const u8* b, size_t n, u32 seed) {
  static u8 ref[32]; unsigned bad = 0; u32 x = seed * 2654435761u + 1;
  for (size_t i = 0; i < n; i += 32) {
    for (int k = 0; k < 32; k += 4) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; std::memcpy(ref + k, &x, 4); }
    if (std::memcmp(ref, b + i, 32)) bad++;
  }
  return bad;
}
static bool dma(u8* a, u8* b, size_t n) {
  C3D_SyncTextureCopy((u32*)a, GX_BUFFER_DIM(512 >> 4, 0), (u32*)b, GX_BUFFER_DIM(512 >> 4, 0), u32(n), GX_TRANSFER_RAW_COPY(1));   // 512-byte lines, raw copy
  GSPGPU_InvalidateDataCache(b, u32(n));
  static int logged = 0;
  if (logged++ < 1) { u32 x, y; std::memcpy(&x, a, 4); std::memcpy(&y, b, 4); n3dsLog("[cache-test] first words: source %08lx copy %08lx\n", (unsigned long)x, (unsigned long)y); }
  return true;
}
static unsigned run(const char* tag) {
  const size_t N = 512u << 10;
  u8* a = static_cast<u8*>(linearAlloc(N)); u8* b = static_cast<u8*>(linearAlloc(N));
  if (!a || !b) { if (a) linearFree(a); if (b) linearFree(b); n3dsLog("[cache-test] %s: no linear memory\n", tag); return 0; }
  unsigned total = 0, r[4] = {};
  // 1: written here, flushed, copied
  std::memset(b, 0x5A, N); GSPGPU_FlushDataCache(b, N);
  fill(a, N, 11); GSPGPU_FlushDataCache(a, N); dma(a, b, N); r[0] = compare(b, N, 11);
  // 2: rewritten after an earlier flush (memory reused for a new texture)
  fill(a, N, 22); GSPGPU_FlushDataCache(a, N); fill(a, N, 23); GSPGPU_FlushDataCache(a, N); dma(a, b, N); r[1] = compare(b, N, 23);
  // 3: written on core 2 and core 3 (other threads), flushed here
  for (int core = 2; core <= 3; core++) {
    Job j{a, N, u32(30 + core)};
    Thread t = threadCreate(fillThread, &j, 0x1000, 0x18, core, false);
    if (!t) { n3dsLog("[cache-test] %s: core %d refused\n", tag, core); continue; }
    threadJoin(t, U64_MAX); threadFree(t);
    GSPGPU_FlushDataCache(a, N); dma(a, b, N); r[core] = compare(b, N, u32(30 + core));
  }
  // 4: small flushes (per 4 KB, like many small textures)
  fill(a, N, 44); for (size_t o = 0; o < N; o += 4096) GSPGPU_FlushDataCache(a + o, 4096); dma(a, b, N); const unsigned r4 = compare(b, N, 44);
  total = r[0] + r[1] + r[2] + r[3] + r4;
  n3dsLog("[cache-test] %s: mismatched 32-byte lines of %u: same core %u, rewritten %u, core2 %u, core3 %u, 4KB flushes %u\n", tag, unsigned(N / 32), r[0], r[1], r[2], r[3], r4);
  linearFree(a); linearFree(b);
  return total;
}
static void runAndFix() {
  if (std::getenv("N3DS_NO_CACHE_TEST")) return;
  const unsigned bad = run("L2 as configured");
  bool isNew = false;
  if (!bad || R_FAILED(APT_CheckNew3DS(&isNew)) || !isNew) return;
  if (R_FAILED(ptmSysmInit())) return;
  const Result rc = PTMSYSM_ConfigureNew3DSCPU(1);   // 804 MHz, L2 off
  n3dsLog("[cache-test] L2 cache off (configure %08lx)\n", (unsigned long)rc);
  const unsigned bad2 = run("L2 off");
  if (bad2 >= bad) { PTMSYSM_ConfigureNew3DSCPU(3); n3dsLog("[cache-test] L2 off did not help: L2 back on\n"); }
  else n3dsLog("[cache-test] keeping L2 off\n");
  ptmSysmExit();
}
}  // namespace cachetest

int n3dsGpuInitLocal(void) {
  const char* presentFence=std::getenv("N3DS_PRESENT_FENCE");
  g_measurePresent=presentFence && !std::strcmp(presentFence,"1");
  const char* texDiag=std::getenv("N3DS_TEX_DIAG");
  g_textureDiag=texDiag && !std::strcmp(texDiag,"1");
  if (const char* fl = std::getenv("N3DS_FRAME_LOG")) {
    unsigned a = 0, b = 0;
    if (std::sscanf(fl, "%u-%u", &a, &b) == 2 && a >= 1 && b >= a) {
      g_frameLogFrom = a; g_frameLogTo = std::min<uint32_t>(b, a + kFrameLogMax - 1);
      const char* fd = std::getenv("N3DS_FRAME_LOG_DRAWS");
      g_frameLogDraws = fd && !std::strcmp(fd, "1");
      n3dsLog("[frame] per-frame log frames %u-%u draw timing %d\n", g_frameLogFrom, g_frameLogTo, int(g_frameLogDraws));
    }
  }
  g_t2pProfOn = std::getenv("N3DS_PROF_DECODE") != nullptr;
  if (g_t2pProfOn) { const unsigned long n = std::strtoul(std::getenv("N3DS_PROF_DECODE"), nullptr, 10); g_t2pProfPeriod = n > 1 ? uint32_t(n) : 1u; n3dsLog("[prof] decode profile: every %lu-th GX draw timed\n", (unsigned long)g_t2pProfPeriod); }   // (Claude v94) N3DS_PROF_DECODE=32: sampled   // per-draw decode / sink timing ("[prof]" lines); costs ~1.3 us per tick, ~5 per GX draw
  if (!C3D_Init(0x200000)) { n3dsLog("[3ds] C3D_Init failed\n"); return 0; }
  cachetest::runAndFix();   // (Claude v101)
  coreprobe::run("boot");   // (Claude v94) "[core-probe]": per-core speed (core 3 = FIFO worker, core 2 = renderer)
  if (std::getenv("N3DS_DEPTH_PROBE")) depthprobe::run();   // (Claude v85) "[depth-probe]" lines; v90: only on request (env.txt N3DS_DEPTH_PROBE=1), its findings are in pipeline.h
  g_tgt = C3D_RenderTargetCreate(TW, TH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
  if (!g_tgt) { n3dsLog("[3ds] render target failed\n"); return 0; }
  C3D_RenderTargetSetOutput(g_tgt, GFX_TOP, GFX_LEFT, GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
  t2p::RendererOptions ro;
  ro.frame.efbW = 640; ro.frame.efbH = 480; ro.frame.targetW = 400; ro.frame.targetH = 240;
  ro.frame.fit = g_perf.widescreen?t2p::FitMode::Stretch:t2p::FitMode::Pillarbox43; ro.frame.widescreen = g_perf.widescreen; ro.frame.rotate = true;
  ro.materialCacheFastPath = g_perf.materialCache;
  ro.translate.packedUniforms = g_perf.packedUniforms;
  ro.texCache.memoPalettes = g_perf.paletteCache;
  ro.packetWorker=g_perf.packetWorker;ro.packetWorkerVerify=g_perf.packetWorkerVerify;ro.planNotes=g_perf.planNotes;ro.directPack=g_perf.directPack;ro.matrixBatch=g_perf.matrixBatch;ro.planTemplates=g_perf.planTemplates;
  ro.packetNotes = false;               // the draw log below never reads them
  ro.texCache.directEncode=g_perf.directTexture;
  ro.texCache.fastLossyError=g_perf.texErrorTable;
  ro.texCache.budgetBytes = 8u << 20;   // Same18MiB linear heap: 6MiB arena +2MiB commands, with the rest for targets/textures.
  t2p::g_packetWorkerCore=g_perf.packetWorkerCore;
  n3dsFastAllocEnable(g_perf.fastAlloc, g_perf.fastAllocSpans);
  g_r = new t2p::Renderer3ds;
  if (!g_r->init(ro, &g_mem, g_tgt)) { n3dsLog("[3ds] renderer init failed\n"); return 0; }
  if(g_perf.drawReuse)g_r->setDrawReuse(true,size_t(g_perf.drawReuseKb)<<10);n3dsLog("[perf] draw_reuse %d budget %d KB (linear free %lu KB)\n",int(g_perf.drawReuse),g_perf.drawReuseKb,(unsigned long)(linearSpaceFree()>>10));
  n3dsLog("[sizes] event %u state %u draw-state %u draw %u copy %u bytes\n",unsigned(sizeof(t2p::RecordedEvent3ds)),unsigned(sizeof(t2p::GxState)),unsigned(sizeof(t2p::GxDrawState)),unsigned(sizeof(t2p::GxDraw)),unsigned(sizeof(t2p::GxCopy)));
  t2p::g_packPlanar=g_perf.planarPack;n3dsLog("[perf] planar_pack %d auto_ab %d\n",int(g_perf.planarPack),g_perf.autoAb);
  t2p::g_packVertexMajor=g_perf.packVertexMajor;n3dsLog("[perf] pack_vertex_major %d\n",int(g_perf.packVertexMajor));
  {const char* k=std::getenv("N3DS_GPU_KICK");if(k)g_perf.gpuKick=std::atoi(k);}   // (Claude v120) A/B
  g_r->backend().setKickDraws(g_perf.gpuKick);n3dsLog("[perf] gpu_kick %d (GPU starts during the frame every split / %d draws; 0 = at frame end)\n",g_perf.gpuKick,g_perf.gpuKick);
  g_r->backend().setSplitStep(g_perf.gpuSplit/100.0f);n3dsLog("[perf] gpu_split %d%% of the command buffer per GPU hand-off\n",g_perf.gpuSplit);
  n3dsLog("[packet-worker] requested %d core %d active %d verify %d\n",int(g_perf.packetWorker),g_perf.packetWorkerCore,int(g_r->packetWorkerActive()),int(g_perf.packetWorkerVerify));
  g_r->setLogs([](const t2p::DrawReport& D) {
    static std::vector<std::string> seen;   // each refusal reason once
    if (g_textureDiag && D.planNotes) {
      static std::vector<std::string> notes;
      for (const auto& note : *D.planNotes) {
        if (notes.size()<150 && std::find(notes.begin(),notes.end(),note)==notes.end()) {
          notes.push_back(note);
          n3dsLog("[tex-diag] frame %lu draw %d: %s\n",(unsigned long)g_frames.load(),D.index,note.c_str());
        }
      }
    }
    const double ms = D.buildMs + D.planMs + D.drawMs;
    bool newWhy = false;
    if (!D.drawn && std::find(seen.begin(), seen.end(), D.why) == seen.end() && seen.size() < 100) { seen.push_back(D.why); newWhy = true; }
    if (ms > 150 || newWhy)
      n3dsLog("[draw] #%d %s build %.0f plan %.0f draw %.0f ms verts %u%s%s\n", D.index, D.drawn ? "drawn" : "REFUSED", D.buildMs, D.planMs, D.drawMs,
              unsigned(D.vertices), D.drawn ? "" : ": ", D.why.c_str());
  }, nullptr, [](const std::string& e) {
    g_errCount++;
    if (e != g_errLast && g_errCount < 200) { n3dsLog("[gx] %s\n", e.c_str()); g_errLast = e; }
  });
  if (n3dsRenderThreadActive()) g_r->setCacheVerifiedHook([] { g_mem.verifying=false; });
  if (FILE* sf = std::fopen(N3DS_ROOT "/shots.txt", "r")) {
    unsigned long long f;
    while (std::fscanf(sf, "%llu", &f) == 1) g_shotFrames.push_back(f);
    std::fclose(sf);
    if (!g_shotFrames.empty()) g_shots = std::fopen(N3DS_ROOT "/shots.bin", "wb");
  }
  g_r->setFrameBeginHook([] {   // the previous frame's picture is in the colour buffer now
    static int idState = 0;   // (Claude v101) 1: the frame that just ended was drawn in draw-ID mode, capture it now
    if (idState == 1 && g_tgt) {
      idState = 0;
      g_r->backend().beginDrawIdFrame(false);
      static FILE* xf = [] { FILE* f = std::fopen(N3DS_ROOT "/captures.bin", "ab"); if (f) std::setvbuf(f, nullptr, _IOFBF, 64u << 10); return f; }();
      if (xf) {
        const u8* cb = reinterpret_cast<const u8*>(g_tgt->frameBuf.colorBuf);
        static u8 copy[16384]; const size_t total = size_t(TW) * TH * 4;
        char nm[32] = {}; std::snprintf(nm, sizeof nm, "drawid %llu", (unsigned long long)g_frames.load());
        std::fwrite(nm, 1, sizeof nm, xf);
        for (size_t o = 0; o < total; o += sizeof copy) { const size_t n = std::min(sizeof copy, total - o); std::memcpy(copy, cb + o, n); std::fwrite(copy, 1, n, xf); }
        std::memset(copy, 0, sizeof copy);
        for (size_t o = 0; o < total; o += sizeof copy) { const size_t n = std::min(sizeof copy, total - o); std::fwrite(copy, 1, n, xf); }
        std::fflush(xf);
        n3dsLog("[drawid] capture saved (frame %llu)\n", (unsigned long long)g_frames.load());
      }
    }
    if (g_pendingCapture && g_tgt) {
      if (g_captureCount < 12) { idState = 1; g_r->backend().beginDrawIdFrame(true); n3dsLog("[drawid] frame after capture %llu drawn with draw numbers\n", (unsigned long long)g_pendingCapture); }
      // (Claude v93) 64 KB stdio buffers: with newlib's 1 KB default the v92 texture dump (6.6 MB) became ~6600 SD writes and stalled the game 60 s
      static FILE* cf = [] { FILE* f = std::fopen(N3DS_ROOT "/captures.bin", "ab"); if (f) std::setvbuf(f, nullptr, _IOFBF, 64u << 10); return f; }();
      if (cf && g_captureCount < 40) {
        // (Claude v85) VRAM must never reach fwrite: the FS process cannot map a VRAM buffer for the SD write and the kernel faults (v84: Luma arm11
        // dump 20, data abort in the kernel on behalf of "fs" at the first SELECT). The CPU reads VRAM itself into a heap copy, which is written.
        const u8* cb = reinterpret_cast<const u8*>(g_tgt->frameBuf.colorBuf); const u8* db = reinterpret_cast<const u8*>(g_tgt->frameBuf.depthBuf);
        // (Claude v90) a static 16 KB buffer (not the render thread's stack), written in pieces: no 384 KB heap block kept for the session (the console heap is short at save load)
        static u8 copy[16384]; const size_t total = size_t(TW) * TH * 4;
        char nm[32] = {}; std::snprintf(nm, sizeof nm, "capture %llu", (unsigned long long)g_pendingCapture);
        std::fwrite(nm, 1, sizeof nm, cf);
        for (size_t o = 0; o < total; o += sizeof copy) { const size_t n = std::min(sizeof copy, total - o); std::memcpy(copy, cb + o, n); std::fwrite(copy, 1, n, cf); }
        for (size_t o = 0; o < total; o += sizeof copy) { const size_t n = std::min(sizeof copy, total - o); if (db) std::memcpy(copy, db + o, n); else std::memset(copy, 0, n); std::fwrite(copy, 1, n, cf); }
        std::fflush(cf); g_captureCount++;
        n3dsLog("[capture] saved frame %llu (%u this session, fog range %.0f)\n", (unsigned long long)g_pendingCapture, g_captureCount, g_depthVisibility.fogRange);
        if (g_captureCount <= 1) {   // (Claude v91) and the textures in use (texdump.bin): converted GPU data + the guest bytes behind it, for hardware-only texture bugs (v93: first capture only)
          static FILE* tf = [] { FILE* f = std::fopen(N3DS_ROOT "/texdump.bin", "ab"); if (f) std::setvbuf(f, nullptr, _IOFBF, 64u << 10); return f; }();
          const u64 t0 = svcGetSystemTick();
          const size_t nt = g_r->dumpTextures(tf, g_pendingCapture);
          n3dsLog("[capture] texture dump: %lu textures in %.0f ms\n", (unsigned long)nt, double(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11);
        }
      }
      g_pendingCapture = 0;
    }
    if (!g_pendingShot || !g_shots) return;
    static const bool routeShot = std::getenv("N3DS_UI_ROUTE_SHOT") != nullptr;   // (Claude v80) DIAGNOSTIC: the bottom route target too (ui_route_shots.bin)
    if (routeShot) if (C3D_RenderTarget* b = g_r->uiRouteTarget()) {
      static FILE* rf = std::fopen(N3DS_ROOT "/ui_route_shots.bin", "wb");
      if (rf) { u8* bb = reinterpret_cast<u8*>(b->frameBuf.colorBuf); GSPGPU_InvalidateDataCache(bb, 240 * 320 * 4);
        char nm[32] = {}; std::snprintf(nm, sizeof nm, "bottom %llu", (unsigned long long)g_pendingShot);
        std::fwrite(nm, 1, sizeof nm, rf); std::fwrite(bb, 1, size_t(240) * 320 * 4, rf); std::fflush(rf); }
    }
    u8* buf = reinterpret_cast<u8*>(g_tgt->frameBuf.colorBuf);
    GSPGPU_InvalidateDataCache(buf, TW * TH * 4);
    char name[32] = {};
    std::snprintf(name, sizeof name, "frame %llu", (unsigned long long)g_pendingShot);
    std::fwrite(name, 1, sizeof name, g_shots);
    std::fwrite(buf, 1, size_t(TW) * TH * 4, g_shots);
    std::fflush(g_shots);
    n3dsLog("[3ds] saved %s\n", name);
    g_pendingShot = 0;
  });
  g_r->fifo().setGuestArrayReader([](uint32_t addr, uint32_t size) { return g_mem.read(addr, size); });
  t2p::g_picaDepthFold=g_perf.depthFold;n3dsLog("[perf] depth_fold %d\n",int(g_perf.depthFold));
  t2p::g_picaDepthReverse=g_perf.depthReverse;n3dsLog("[perf] depth_reverse %d\n",int(g_perf.depthReverse));   // (Claude v107)
  t2p::g_picaForceZDepth=g_perf.depthZ;n3dsLog("[perf] depth_z %d%s\n",int(g_perf.depthZ),g_perf.depthZ?" (EXPERIMENT: Z depth everywhere, fog off)":"");   // (Claude v93)   // (Claude v90)
  n3dsLog("[build-fix] console-return 2026-11-16: DMA boundary handoff, live map settings, matrix validity, draw-state capture\n");
  if(g_perf.preparedMaterials){sPreparedMaterialsEnabled=sPreparedMaterials.open(N3DS_ROOT "/ww_materials.bin");if(sPreparedMaterialsEnabled)sMaterialPrograms.setBudget(2u<<20);}
#if defined(N3DS_EVENT_INDIRECT) && N3DS_EVENT_INDIRECT
  n3dsLog("[event-storage] indirect 1 event %lu state %lu bytes\n",(unsigned long)sizeof(t2p::RecordedEvent3ds),(unsigned long)sizeof(t2p::DeferredDrawState));
#else
  n3dsLog("[event-storage] indirect 0 event %lu state %lu bytes\n",(unsigned long)sizeof(t2p::RecordedEvent3ds),(unsigned long)sizeof(t2p::DeferredDrawState));
#endif
  n3dsLog("[material-pack] requested %d loaded %d programs %lu\n",int(g_perf.preparedMaterials),int(sPreparedMaterialsEnabled),(unsigned long)sPreparedMaterials.programs());
  g_n3dsMaterialIdentity=g_perf.modelMaterialIds||sPreparedMaterialsEnabled;
  g_n3dsModelDirect=g_perf.modelDirect;n3dsLog("[model] direct %d\n",int(g_perf.modelDirect));
  g_n3dsMaterialRecords=g_perf.materialRecords;n3dsLog("[model] material_records %d\n",int(g_perf.materialRecords));
  g_n3dsModelRecords=g_perf.modelRecords;n3dsLog("[model] model_records %d\n",int(g_perf.modelRecords));
  t2p::g_sharedMaterialVectors=g_perf.sharedMaterials;n3dsLog("[model] shared_materials %d\n",int(g_perf.sharedMaterials));
  t2p::g_uniformSections=g_perf.uniformSections;
  t2p::g_stateCpIncremental=g_perf.stateCpIncremental;n3dsLog("[state] cp incremental %d\n",int(g_perf.stateCpIncremental));
  t2p::g_uniformPieces=g_perf.uniformPieces;n3dsLog("[uniform] pieces %d\n",int(g_perf.uniformPieces));
  t2p::g_compactPoseStorage=g_perf.compactPoseStorage;n3dsLog("[model] compact_pose %d\n",int(g_perf.compactPoseStorage));
  if (g_perf.texturePack) {   // (Claude v92) offline texture pack
    const bool verify = std::getenv("N3DS_TEXPACK_VERIFY") != nullptr;
    const bool ok = g_r->loadTexturePack(g_perf.textureShards?N3DS_ROOT "/ww_texpack":N3DS_ROOT "/ww_texpack.bin", verify, g_perf.textureShards);
    n3dsLog("[texstore] split %d page 8 KB\n",int(g_perf.textureShards));
    n3dsLog("[texpack] %s: %lu entries%s\n", ok ? "loaded" : "not loaded (missing or built with other options): textures are converted here", (unsigned long)g_r->texturePackEntries(), verify ? ", VERIFY (every hit is converted too)" : "");
  } else n3dsLog("[texpack] off (texture_pack=0)\n");
  g_r->setDirectEvents(g_perf.directEvents||g_perf.modelDirect,g_perf.modelDirect?512:g_perf.directSlots);g_r->setDirectGroups(g_perf.modelDirect);g_r->setDirectSnapshots(g_perf.directSnapshot);n3dsLog("[direct-snapshot] enabled %d\n",int(g_perf.directSnapshot));n3dsLog("[direct] direct_events %d slots %d\n",int(g_perf.directEvents),g_perf.directSlots);
  g_r->setModelPrograms(g_perf.modelPrograms);n3dsLog("[model] model_programs %d\n",int(g_perf.modelPrograms));
  g_r->setUniformDelta(g_perf.uniformDelta);n3dsLog("[model] uniform_delta %d\n",int(g_perf.uniformDelta));
  if(g_perf.preparedDraw)n3dsLog("[prepared] draw commands loaded %d\n",int(g_r->backend().loadPreparedDraw(N3DS_ROOT "/ww_commands.cwcp")));
  g_r->setCommandBlocks(g_perf.commandBlocks);n3dsLog("[model] command_blocks %d\n",int(g_perf.commandBlocks));
  g_r->setRigidClip(g_perf.rigidClip);n3dsLog("[clip] rigid_clip %d\n",int(g_perf.rigidClip));
  g_r->setPlanReuse(g_perf.planReuse);
  g_r->setPlanDelta(g_perf.planReuse&&g_perf.planDelta,std::getenv("N3DS_PLAN_DELTA_VERIFY")!=nullptr);   // (Claude v87)
  n3dsLog("[perf] plan_delta %d\n",int(g_perf.planReuse&&g_perf.planDelta));
  if(g_perf.uiMapBottom && g_perf.bottomUi) g_r->setUiRoute(true,[](bool owns){   // (Claude v80) render owner: the bottom screen changes hands
    g_routeOwnsBottom.store(owns,std::memory_order_release);n3dsUiSetGpuOwnsBottom(owns?1:0);
    n3dsLog("[ui-route] bottom screen %s\n",owns?"taken by the map route":"given back");});
  n3dsLog("[perf] ui_map_bottom %d\n",int(g_perf.uiMapBottom && g_perf.bottomUi));
  g_r->setStateNotes(!g_perf.leanState);
  g_r->setRetainGeometry(g_perf.retainGeometry);   // (Claude v83) the renderer's pack cache follows the recorder's retained geometry
  if(g_perf.retainGeometry&&g_perf.residentGeometry)g_r->setResidentGeometry(true,size_t(g_perf.residentKb)<<10);   // (Claude v86) needs retained geometry ids
  { const char* v=getenv("N3DS_DRAW_REPLAY"); const bool on=v?(*v=='1'):g_perf.drawReplay; if(g_perf.retainGeometry&&g_perf.residentGeometry&&on)g_r->setDrawReplay(true,size_t(std::min(g_perf.replayKb,1536))<<10); n3dsLog("[replay] draw_replay %d\n",int(g_r->drawReplay())); }   // (Claude v117)
  { const char* v=getenv("N3DS_DRAW_RECORD"); g_r->setDrawRecord(v?(*v=='1'):g_perf.drawRecord); n3dsLog("[replay] draw_record %d\n",int(g_r->drawRecord())); }   // (Claude v119)
  { const char* v=getenv("N3DS_FLUSH_ALL"); g_r->setFlushAllAtEnd(v?(*v=='1'):g_perf.frameFlushAll); n3dsLog("[flush] frame_flush_all %d\n",int(g_r->flushAllAtEnd())); }   // (Claude v118)
  n3dsLog("[perf] resident_geometry %d budget %d KB (linear free %lu KB)\n",int(g_perf.retainGeometry&&g_perf.residentGeometry),g_perf.residentKb,(unsigned long)(linearSpaceFree()>>10));
  g_r->setStateInPlace(g_perf.stateInPlace);
  g_r->setStatePool(g_perf.statePool);
  g_r->setVertexPoolFifo(g_perf.vertexPoolFifo);
  g_r->setFogPreparation(g_perf.fogPreparation);
  g_r->fifo().setPrimitiveRuns(g_perf.vertexRuns);
  g_r->fifo().setGeometryCache(g_perf.geometryCache && !g_pipeline);
  g_r->fifo().setFastAttributeDecode(g_perf.vertexDecode);
  g_r->fifo().setCachedVertexPlan(g_perf.vertexPlan);
  g_r->fifo().setAuroraMode(true);
  g_r->fifo().setUncheckedArrays(true);
  g_r->fifo().setStopAtDrawDone(true);
  if (n3dsRenderThreadActive() && !g_pipeline) {
    if (!g_mem.snapshot.init(3u<<20)) n3dsLog("[render-thread] no snapshot memory; frames will stay synchronous\n");
    g_r->fifo().setSink(&g_ranges);
    g_r->setCacheVerifiedHook([] { g_mem.verifying=false; });
  }

  {   // materials translated offline (mkbundle): every ww_bundle*.bin in N3DS_ROOT, ww_bundle_misses.bin being what earlier sessions had to search for
    // here (also kept in the recorder, so that the file keeps growing). Missing ones are translated here and recorded.
    std::vector<std::string> names;
    if (DIR* d = opendir(N3DS_ROOT)) {
      while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() >= 13 && n.compare(0, 9, "ww_bundle") == 0 && n.compare(n.size() - 4, 4, ".bin") == 0) names.push_back(n);
      }
      closedir(d);
    }
    // (Claude, release prep) N3DS_NO_BUNDLES=1 (diagnostic): start with no offline materials, as a player without bundle files would, and
    // do not write ww_bundle_misses.bin (the test must not change the SD's accumulated file)
    if (const char* v = getenv("N3DS_NO_BUNDLES"); v && *v == '1') { names.clear(); g_noBundleWrites = true; n3dsLog("[3ds] material bundles: off (N3DS_NO_BUNDLES)\n"); }
    // (ChatGPT patch, applied by Claude) Keep existing cache/title winners; the previously omitted base is a fallback.
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
      const bool baseA = a == "ww_bundle.bin", baseB = b == "ww_bundle.bin";
      if (baseA != baseB) return !baseA;
      return a < b;
    });
    for (const std::string& n : names) {
      std::vector<uint8_t> b;
      if (FILE* bf = std::fopen((std::string(N3DS_ROOT "/") + n).c_str(), "rb")) {
        uint8_t tmp[1 << 14]; size_t k;
        while ((k = std::fread(tmp, 1, sizeof tmp, bf)) > 0) b.insert(b.end(), tmp, tmp + k);
        std::fclose(bf);
      }
      std::string err;
      const bool ok = !b.empty() && g_r->loadMaterialBundle(b.data(), b.size(), &err);
      if (ok && n == "ww_bundle_misses.bin") g_r->loadMissBundle(b.data(), b.size());
      n3dsLog("[3ds] material bundle %s: %s (%zu entries in all) %s\n", n.c_str(), ok ? "loaded" : b.empty() ? "empty" : "refused", g_r->bundleEntries(), err.c_str());
    }
    g_missesWritten = g_r->missEntries();
    g_r->recordBundleMisses(true);
  }
  return 1;
}

static unsigned g_processCalls = 0;   // n3dsGpuProcess calls this frame
uint32_t n3dsGpuProcessLocal(const uint8_t* data, uint32_t size, int* drawDone) {
  const u64 t0 = svcGetSystemTick();
  g_processCalls++;
  size_t used = g_r->process(data, size);
  g_processTicks += svcGetSystemTick() - t0;
  *drawDone = g_r->fifo().takeDrawDone() ? 1 : 0;
  if (used == 0 || (used < size && !*drawDone)) used = size;   // a command the decoder refused: skip the rest of the chunk (reported through the error log)
  return uint32_t(used);
}

// (Claude) decode_helper=1: the recorder's deferred vertex decode (gxfifo.h setDeferredDecode), its jobs run by this thread.
static LightEvent g_decodeWake;
static std::atomic<bool> g_decodeAsleep{false};
static t2p::GxFifo* g_decodeFifo = nullptr;
extern "C++" void JKRPcBeginHostAlloc();
static void decodeHelperMain(void*) {
  JKRPcBeginHostAlloc();   // (Claude v120) decode jobs allocate host memory, never a block of the game's current JKR heap (never ends: thread_local depth)
  for (;;) {
    while (g_decodeFifo->runDecodeJob()) {}
    bool got = false;
    for (int spin = 0; spin < 4000 && !got; spin++) got = g_decodeFifo->runDecodeJob();
    if (got) continue;
    g_decodeAsleep.store(true);
    if (g_decodeFifo->runDecodeJob()) { g_decodeAsleep.store(false); continue; }
    LightEvent_Wait(&g_decodeWake);
    g_decodeAsleep.store(false);
  }
}
static void decodeHelperWake() { if (g_decodeAsleep.load()) LightEvent_Signal(&g_decodeWake); }
static void startDecodeHelper(t2p::GxFifo& fifo) {
  if (!g_perf.decodeHelper) return;   // (Claude v83) auto_ab measures retained geometry now (console v81: the helper lost, 10.1 vs 11.7 fps)
  if (!g_perf.fifoThread) { n3dsLog("[threads] decode_helper needs fifo_thread=1: off\n"); return; }
  LightEvent_Init(&g_decodeWake, RESET_ONESHOT);
  g_decodeFifo = &fifo;
  fifo.setDeferredDecode(true, decodeHelperWake);
  if (const char* m = std::getenv("N3DS_DEFER_MIN")) g_deferMin = uint32_t(std::atoi(m));
  s32 prio = 0x30;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  int core = g_perf.decodeCore;
  if (const char* pv = std::getenv("N3DS_DECODE_PRIO")) prio = s32(std::strtol(pv, nullptr, 0));   // diagnostic
  if (core == 1) { const Result r = APT_SetAppCpuTimeLimit(80); if (R_FAILED(r)) core = 0; }
  Thread t = threadCreate(decodeHelperMain, nullptr, 32u << 10, prio, core, false);
  if (!t) { fifo.setDeferredDecode(false); n3dsLog("[threads] decode helper: core %d refused: off\n", core); return; }
  n3dsLog("[threads] decode helper prio 0x%lx on core %d\n", (long)prio, core);
}

static void finishRecording(t2p::GxFifo& fifo) {
  fifo.finishDecodes();   // (no-op unless decode_helper) every vertex of the frame is decoded before it is handed on
  // (Claude v83) no configuration is switched by auto_ab now: switching retained geometry off and on every 120 frames kept the off blocks' pooled
  // vertex vectors beside the on blocks' retained geometry and ran the emulated console out of heap (v83c soak, frame 725). [ab] still logs both blocks.
  // Aurora's GXCopyDisp is a no-op: the frame ends here. A display copy of the whole EFB (clearing it with the game's clear colour), then the copy
  // registers it used back as they were.
  const t2p::GxRegs& R = fifo.regs();
  g_xfb.clear();
  auto bpw = [&](uint32_t id, uint32_t val) { g_xfb.push_back(0x61); push32(g_xfb, (id << 24) | (val & 0xFFFFFF)); };
  const uint32_t ids[] = {0x40, 0x41, 0x49, 0x4A, 0x4B, 0x4D};
  uint32_t saved[6]; bool had[6];
  for (int i = 0; i < 6; i++) { saved[i] = R.bp[ids[i]]; had[i] = R.bpWritten[ids[i]]; }
  bpw(0x40, 0x1F); bpw(0x41, 0x18); bpw(0x49, 0); bpw(0x4A, 639u | (479u << 10)); bpw(0x4B, 0x00400000u >> 5); bpw(0x4D, 640u * 2 / 32);
  bpw(0x52, (1u << 14) | (1u << 11));
  for (int i = 0; i < 6; i++) if (had[i]) bpw(ids[i], saved[i]);
  const bool stop = true;
  fifo.setStopAtDrawDone(false);
  fifo.process(g_xfb.data(), g_xfb.size());
  fifo.setStopAtDrawDone(stop);

}

void n3dsGpuEndFrameLocal(void) {
  g_n3dsMark[7]=1;
  struct StageOut { ~StageOut() { g_n3dsMark[7]=0; } } stageOut;
  t2p::CpuFrame3ds* frame=g_pipeline ? g_submitted : nullptr;
  g_renderSlot=g_pipeline ? g_submittedSlot : 0;   // (Claude v120) present pacing
  DepthVisibility3ds depthVisibility;
  if(frame) for(const auto& e:frame->events) if(e.kind==t2p::RecordedEvent3ds::Draw && e.viewState().draw.z.update){
    const auto& v=e.viewState().viewport;const auto& p=e.viewState().proj;
    depthVisibility.add({p.m[2][2],p.m[2][3],p.nearZ,p.farZ,v.nearZ,v.farZ,p.ortho});
  }
  unsigned depthOrtho=0,depthPersp=0;
  const bool depthDiag=std::getenv("N3DS_DEPTH_DIAG")!=nullptr;
  if(depthDiag && frame) for(const auto& e:frame->events) if(e.kind==t2p::RecordedEvent3ds::Draw && e.viewState().draw.z.compare && e.viewState().draw.z.func!=7) {if(e.viewState().proj.ortho)++depthOrtho;else ++depthPersp;}
  if(frame&&g_perf.directSnapshot&&(g_frames.load()+1)%60==0)n3dsLog("[direct-snapshot] frame %lu active %u\n",(unsigned long)(g_frames.load()+1),frame->directDiag[14]);
  if(frame&&std::getenv("N3DS_DIRECT_DIAG")&&(g_frames.load()+1)%60==0){const auto* d=frame->directDiag;n3dsLog("[direct-reasons] frame %lu total %u route %u palette %u no-geometry %u not-requested %u no-program %u matched %u flags %u colors %u texture-address %u BP %u XF %u captured %u\n",(unsigned long)(g_frames.load()+1),d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],d[9],d[10],d[11],d[15]);}
  if (frame) {
    g_r->swapRecording(*frame);
    g_processTicks=frame->processTicks; g_processCalls=frame->processCalls;
    for(int i=0;i<16;++i) g_t2pProf[i]=frame->prof[i];
  } else finishRecording(g_r->fifo());

  char gameHeaps[256] = {};
  if (frame) std::memcpy(gameHeaps,frame->heaps,sizeof gameHeaps);
  else if ((g_frames.load()+1)%60==0) n3dsGameHeapReport(gameHeaps,sizeof gameHeaps);
  g_mem.verifying=true; g_mem.missing=0; g_mem.verifyMissing=0;
  if (g_r3dsTrace) g_r3dsTrace("end-frame: start", 0);
  const bool overlapped=frame ? frame->frozen : n3dsRenderThreadActive() && g_mem.snapshot.freeze(readableTexture,snapshotSkip);
  g_mem.view=frame ? &frame->snapshot : nullptr;
  const uint32_t snapshotBytes=frame ? frame->snapshot.bytes() : g_mem.snapshot.bytes();
  if(frame && (g_frames.load()<5 || (g_frames.load()+1)%60==0)){
    const double k=1000.0/SYSCLOCK_ARM11;
    const uint64_t work=frame->processTicks>frame->processStagingWaitTicks?frame->processTicks-frame->processStagingWaitTicks:0;
    n3dsLog("[cpu-cost] frame %lu process-work %.2f process-staging-wait %.2f all-staging-wait %.2f handoff-wait %.2f ms\n",(unsigned long)(g_frames.load()+1),work*k,frame->processStagingWaitTicks*k,frame->stagingWaitTicks*k,frame->handoffWaitTicks*k);
  }
  g_mem.frozen=overlapped;
  if (overlapped) n3dsRenderThreadRelease();
  else if (n3dsRenderThreadActive()) ++g_snapshotFallbacks;
  const u64 r0 = svcGetSystemTick();
  const uint32_t logFrame = g_frames.load() + 1;   // the number this frame's "[3ds] frame" line carries
  const bool frameLogged = g_frameLogFrom && logFrame >= g_frameLogFrom && logFrame <= g_frameLogTo;
  if (g_frameLogFrom && logFrame == g_frameLogFrom) g_frameLogTex = g_r->cache().stats();
  const bool queueSample=logFrame<=5 || (logFrame<=300 && logFrame%10==0) || logFrame%60==0 || frameLogged;
  g_r->setFrameTiming(queueSample);
  g_r->setDrawTimingEnabled(g_perf.drawTiming || g_frames.load()<5 || (g_frames.load()+1)%60==0 || (frameLogged && g_frameLogDraws));
  // (Claude v77) built-in A/B (auto_ab): both configurations give the same pixels; alternate them in 120-frame blocks so one play session measures both
  const int abSide=g_perf.autoAb?int((logFrame/120)&1):-1;
  // (Claude v83) the A/B switches nothing (see finishRecording); both blocks are logged
  if (g_r3dsTrace) g_r3dsTrace("end-frame: before renderFrame", 0);
  {   // (Claude v97) N3DS_AB_CYCLE: draw reuse switched here, on the render owner, between frames
    static int cur = -1; const int want = g_abDrawReuse.load();
    if (want >= 0 && want != cur) { cur = want; g_r->setDrawReuse(want != 0, size_t(g_perf.drawReuseKb) << 10); }
  }
  const t2p::FrameReport F = g_r->renderFrame(g_r->framesReady() == 0);
  depthVisibility.fogRange=F.fogRange;g_depthVisibility=depthVisibility;
  if(depthDiag && (g_frames.load()+1)%60==0) n3dsLog("[depth-order] frame %lu fog-range %.1f tested ortho %u perspective %u\n",(unsigned long)(g_frames.load()+1),F.fogRange,depthOrtho,depthPersp);
  if (g_r3dsTrace) g_r3dsTrace("end-frame: after renderFrame", 0);
  const uint64_t finishTick=queueSample?svcGetSystemTick():0;
  if (g_measurePresent) C3Di_RenderQueueWaitDone();
  const uint64_t finishWaitTicks=queueSample?svcGetSystemTick()-finishTick:0;
  const double renderMs = double(svcGetSystemTick() - r0) * 1000.0 / SYSCLOCK_ARM11, processMs = double(g_processTicks) * 1000.0 / SYSCLOCK_ARM11;
  if(F.packetJobs && (g_frames.load()<5 || (g_frames.load()+1)%60==0))n3dsLog("[packet-worker] frame %lu jobs %u wait %.2f ms checks %u mismatches %u\n",(unsigned long)(g_frames.load()+1),F.packetJobs,F.packetWaitTicks*1000.0/SYSCLOCK_ARM11,F.packetChecks,F.packetMismatches);
  const u64 presented=svcGetSystemTick();
  const double presentMs=g_lastPresent ? double(presented-g_lastPresent)*1000.0/SYSCLOCK_ARM11 : 0;
  g_lastPresent=presented;
  if (g_r3dsTrace) g_r3dsTrace("end-frame: bottom ui", 0);
  if(g_perf.bottomUi) g_bottomUi.update((unsigned long)(g_frames.load()+1),g_presentCount ? g_presentSum/g_presentCount : presentMs);
  {  // (Claude v77) [ab]: per side, frames with at least 100 draws (gameplay), skipping the first 10 frames of each block (pipeline settling)
    static double abRender[2],abPresent[2],abProcess[2],abBuild[2],abPlan[2],abDraw[2];static uint32_t abFrames[2],abTimed[2];
    if(abSide>=0 && F.draws>=100 && logFrame%120>=10 && presentMs>0){abRender[abSide]+=renderMs;abPresent[abSide]+=presentMs;abProcess[abSide]+=processMs;++abFrames[abSide];
      if(logFrame%60==0){abBuild[abSide]+=F.buildMs;abPlan[abSide]+=F.planMs;abDraw[abSide]+=F.drawMs;++abTimed[abSide];}}
    if(abSide>=0 && logFrame%1200==0){
      for(int s=0;s<2;s++){const double n=abFrames[s]?abFrames[s]:1,m=abTimed[s]?abTimed[s]:1;
        n3dsLog("[ab] frame %lu side %d (%s): %u frames render %.2f present %.2f (%.2f fps) process %.2f ms; %u timed: build %.2f plan %.2f draw %.2f ms\n",(unsigned long)logFrame,s,s?"block B":"block A",abFrames[s],abRender[s]/n,abPresent[s]/n,abPresent[s]>0?1000.0*n/abPresent[s]:0.0,abProcess[s]/n,abTimed[s],abBuild[s]/m,abPlan[s]/m,abDraw[s]/m);}
    }
  }
  if(presentMs>0){g_presentSum+=presentMs;++g_presentCount;g_presentMin=std::min(g_presentMin,presentMs);g_presentMax=std::max(g_presentMax,presentMs);}
  g_mem.frozen=false;
  if (g_r3dsTrace) g_r3dsTrace("end-frame: after bottom ui", 0);
  if (frame) { g_r->swapRecording(*frame); frame->snapshot.reset(); }
  if (g_r3dsTrace) g_r3dsTrace("end-frame: recording swapped", 0);
  else g_mem.snapshot.reset();
  g_mem.view=nullptr;
  if (g_mem.missing) n3dsLog("[render-thread] ERROR missing current texture snapshots %lu\n",(unsigned long)g_mem.missing);
  g_processTicks = 0;
  const unsigned processCalls = g_processCalls;
  g_processCalls = 0;
  g_frames++;
  if(g_frames%60==0 && g_presentCount){
    n3dsLog("[present] frame %lu: %u intervals mean %.1f ms (%.2f fps) min %.1f max %.1f fence %d pipeline %d\n",
      (unsigned long)g_frames.load(),g_presentCount,g_presentSum/g_presentCount,1000.0*g_presentCount/g_presentSum,g_presentMin,g_presentMax,int(g_measurePresent),int(g_pipeline));
    g_presentSum=0;g_presentCount=0;g_presentMin=1e30;g_presentMax=0;
  }
  for (uint64_t f : g_shotFrames) if (f == g_frames && F.rendered) g_pendingShot = f;
  {   // (Claude v91) TESTING: env N3DS_CAPTURE_AT=frame acts like SELECT at that frame (autopress bypasses hidKeysDown)
    static const unsigned long capAt = [] { const char* s = std::getenv("N3DS_CAPTURE_AT"); return s ? std::strtoul(s, nullptr, 10) : 0ul; }();
    if (capAt && g_frames == capAt) g_captureRequest.store(true);
  }
  if (F.rendered && g_captureRequest.load(std::memory_order_relaxed)) { g_captureRequest.store(false); g_pendingCapture = g_frames; }
  if (g_t2pProfOn && g_frames % 60 == 0) {
    const double k = 1000.0 * g_t2pProfPeriod / SYSCLOCK_ARM11;   // (Claude v94) sampled draws scaled up by the period
    { n3dsLog("[prof] state decodes per frame: new batch %llu, matrix growth %llu\n", (unsigned long long)g_n3dsDecodeCount[0], (unsigned long long)g_n3dsDecodeCount[1]); g_n3dsDecodeCount[0] = g_n3dsDecodeCount[1] = 0; }
    n3dsLog("[prof-frontend] per frame: pre-append %.2f close+charge %.2f event-create %.2f attribute-setup %.2f retention-offer %.2f ms\n",g_t2pProf[9]*k,g_t2pProf[12]*k,g_t2pProf[13]*k,g_t2pProf[14]*k,g_t2pProf[7]*k);
    n3dsLog("[prof] per frame (1/%lu sampled): vertex decode %.1f ms (%llu verts), sink %.1f ms (state decode %.1f, draw copy %.1f, append %.1f); process %.1f ms\n", (unsigned long)g_t2pProfPeriod, g_t2pProf[0] * k, (unsigned long long)(g_t2pProf[2] * g_t2pProfPeriod),
            g_t2pProf[1] * k, g_t2pProf[3] * k, g_t2pProf[4] * k, g_t2pProf[5] * k, double(processMs));
  }
  if (g_t2pProfOn && g_frames % 60 == 0) {   // (Claude v100) state decode by section (recorder thread's counters; read racily, diagnostic)
    const double k = 1000.0 * g_t2pProfPeriod / SYSCLOCK_ARM11;
    n3dsLog("[prof-state] per frame: copy-prev %.1f tev %.1f vertex-pipe %.1f uniforms %.1f pixel+viewport %.1f textures %.1f ms\n", g_recStateProf[5]*k, g_recStateProf[0]*k, g_recStateProf[1]*k, g_recStateProf[2]*k, g_recStateProf[3]*k, g_recStateProf[4]*k);
  }
  if(g_t2pProfOn) for (uint64_t& v : g_t2pProf) v = 0;
  if (frame && g_frames%60==0)
    n3dsLog("[pipeline] frame %lu: estimated staging %lu KB fence %d heap checks %u peak in use %lu KB min sbrk room %ld KB compact %lu KB/%u\n",(unsigned long)g_frames.load(),(unsigned long)(frame->stagedBytes>>10),int(frame->stagingFence),frame->heapChecks,(unsigned long)(frame->heapPeakUse>>10),g_minBrkRoom==~size_t(0)?-1L:long(g_minBrkRoom>>10),(unsigned long)(frame->compactSavedBytes>>10),frame->compactBatches);
    if (g_decodeFifo) { static unsigned long p[8]; unsigned long d[8]; for(int k=0;k<8;k++){d[k]=g_deferWhy[k]-p[k];p[k]=g_deferWhy[k];} n3dsLog("[decode-why] per 60 frames: fast draws %lu, queued %lu (below minimum %lu), run by worker itself %lu; attribute-vertices all %lu queued %lu; min %lu\n",d[0],d[5],d[3],d[1],d[6],d[7],(unsigned long)g_deferMin); }
    if (g_decodeFifo) { static uint64_t pj=0,ps=0,pd=0; const uint64_t j=g_decodeFifo->deferredJobs(),sf=g_decodeFifo->deferredSelfRun(),dr=g_decodeFifo->deferredDrains(); n3dsLog("[decode-helper] since last: jobs %lu, run by the worker itself %lu, drains with work pending %lu\n",(unsigned long)(j-pj),(unsigned long)(sf-ps),(unsigned long)(dr-pd)); pj=j;ps=sf;pd=dr; }
  if (frame && g_frames%60==0) g_minBrkRoom=~size_t(0);
  { static const bool dlCensus = std::getenv("N3DS_DL_CENSUS") != nullptr;   // (Claude v92) DIAGNOSTIC: display lists by content, frames 600..700
    if (dlCensus && g_frames == 600) g_n3dsDlCensusOpen = 1;
    if (dlCensus && g_frames == 700) { g_n3dsDlCensusOpen = 0; n3dsDlCensusWrite(N3DS_ROOT "/dlcensus.txt"); n3dsLog("[dl-census] written\n"); } }
  if(g_frames%120==0)n3dsAudioHealth();
  if(g_perf.texturePack&&g_perf.textureShards&&g_frames%60==0){const auto& s=g_r->textureStoreStats();n3dsLog("[texstore] reads %llu bytes %llu opens %llu hits %llu\n",(unsigned long long)s.reads,(unsigned long long)s.bytes,(unsigned long long)s.opens,(unsigned long long)s.hits);}
  if(g_perf.rigidClip && g_frames%60==0)n3dsLog("[clip] frame %lu tests %u rejected %u\n",(unsigned long)g_frames.load(),F.clipTests,F.clipCulled);
  if (g_frames % 600 == 0) {   // (Claude v92) texture pack effect: misses served from the pack vs converted here, and the time each took
    const t2p::TexCacheStats ts = g_r->cache().stats(); const double k = 1000.0 / SYSCLOCK_ARM11;
    n3dsLog("[texpack] frame %lu: pack hits %llu (read %.0f ms, %.1f MB), converted %llu (%.0f ms), read failures %llu, verified %llu bad %llu\n", (unsigned long)g_frames.load(),
            (unsigned long long)ts.packHits, ts.packReadTicks * k, double(ts.packReadBytes) / 1e6, (unsigned long long)(ts.misses - ts.packHits), ts.convTicks * k, (unsigned long long)ts.packReadFail,
            (unsigned long long)ts.packVerified, (unsigned long long)ts.packVerifyBad);
    n3dsLog("[texpack-cost] cumulative: SD seek/read %.1f ms, output allocation %.1f ms\n", ts.packIoTicks * k, ts.packAllocTicks * k);
  }
  { static const bool texProf = [] { const char* v = std::getenv("N3DS_PROF_TEX"); return v && v[0] == '1'; }();
    if (texProf && g_frames % 60 == 0) {   // (Claude) texture-miss stage times over the last 60 frames (render side; both cores' misses)
      const double k = 1000.0 / SYSCLOCK_ARM11; uint64_t* t = t2p::g_texProf;
      n3dsLog("[texprof] frame %lu: %llu conversions (%llu lossy) total %.1f ms: decode %.1f, exact pick %.1f, lossy trial %.1f; upload %.1f ms\n", (unsigned long)g_frames.load(),
              (unsigned long long)t[5], (unsigned long long)t[6], t[0] * k, t[1] * k, t[2] * k, t[3] * k, t[4] * k);
      for (int i = 0; i < 8; i++) t[i] = 0;
    } }
  if (g_gxOriginProf && g_frames % 60 == 0) {
    const double k = 1000.0 / SYSCLOCK_ARM11 / 60.0;
    for (int o = 0; o < 2; o++) {
      const uint64_t* c = g_gxOriginCount[o];
      n3dsLog("[origin] frame %lu %s: process %.1f ms/frame, %llu calls, %llu KB, draws %llu, verts %llu, BP %llu CP %llu XF %llu (%llu words) idxXF %llu per frame\n",
              (unsigned long)g_frames.load(), o ? "displaylist" : "typed", g_originTicks[o] * k, (unsigned long long)(g_originCalls[o] / 60),
              (unsigned long long)(g_originBytes[o] / 60 >> 10), (unsigned long long)(g_originDraws[o] / 60), (unsigned long long)(g_originVerts[o] / 60),
              (unsigned long long)(c[0] / 60), (unsigned long long)(c[1] / 60), (unsigned long long)(c[2] / 60), (unsigned long long)(c[3] / 60), (unsigned long long)(c[4] / 60));
      g_originTicks[o] = g_originBytes[o] = g_originDraws[o] = g_originVerts[o] = g_originCalls[o] = 0;
      for (int i = 0; i < 5; i++) g_gxOriginCount[o][i] = 0;
    }
  }
  if (g_frames % 60 == 0) {   // memory: the host heap (renderer, caches; operator new inside HostAlloc scopes) and MEM1's heaps
    extern u32 __ctru_heap_size;
    const struct mallinfo mi = mallinfo();

    n3dsLog("[mem] host heap: in use %u KB, taken from the system %u KB of %lu KB (sbrk room %u KB); %s\n", unsigned(mi.uordblks >> 10), unsigned(mi.arena >> 10),
            (unsigned long)(__ctru_heap_size >> 10), unsigned(brkRoom() >> 10), gameHeaps);
    if (g_r && g_r->drawReplay()) {   // (Claude v117) draw replay grows into heap room above 7 MiB free (the staging fence wants 6), up to replay_kb, never below 512 KB
      const size_t freeB = brkRoom() + size_t(mi.fordblks), used = g_r->drawReplayBytes(), keep = 7u << 20;
      const size_t want = std::max<size_t>(512u << 10, std::min<size_t>(size_t(g_perf.replayKb) << 10, freeB > keep ? used + (freeB - keep) : used / 2));
      g_r->setDrawReplayBudget(want);
      n3dsLog("[replay] budget %u KB (entries %u KB, heap free %u KB)\n", unsigned(want >> 10), unsigned(used >> 10), unsigned(freeB >> 10));
    }
    n3dsBigAllocReport();
    if (g_r && g_r->backend().stats().copyAllocFails) n3dsLog("[copy] rendered-texture allocations failed so far: %u (copy lost, clear kept); VRAM free %u KB, render-target pool %u\n", unsigned(g_r->backend().stats().copyAllocFails), unsigned(vramSpaceFree() / 1024), unsigned(g_r->backend().rtPoolSize()));   // (Claude, shadow fix)
    n3dsLog("[fifo] drain stalls %u\n", n3dsFifoDrainStalls);
  }
  if (frameLogged && g_frameLogCount < kFrameLogMax) {
    const t2p::TexCacheStats ts = g_r->cache().stats();
    FrameSample& fs = g_frameLog[g_frameLogCount++];
    fs = FrameSample{logFrame, unsigned(F.drawn), unsigned(F.gxDraws), uint32_t(ts.misses - g_frameLogTex.misses), uint32_t(ts.conversions - g_frameLogTex.conversions),
                     uint32_t(ts.evicted - g_frameLogTex.evicted), uint32_t(ts.bytes >> 10), float(processMs), float(renderMs), float(F.buildMs), float(F.planMs), float(F.drawMs),
                     float(presentMs), presented};
    g_frameLogTex = ts;
  }
  if (g_frameLogFrom && logFrame == g_frameLogTo) {
    for (uint32_t i = 0; i < g_frameLogCount; i++) {
      const FrameSample& fs = g_frameLog[i];
      n3dsLog("[frame] %lu draws %lu gx %lu process %.2f render %.2f build %.2f plan %.2f draw %.2f present %.2f tick %llu tex miss %lu conv %lu evict %lu KB %lu\n",
              (unsigned long)fs.frame, (unsigned long)fs.drawn, (unsigned long)fs.gx, fs.process, fs.render, fs.build, fs.plan, fs.draw, fs.present,
              (unsigned long long)fs.presentTick, (unsigned long)fs.texMiss, (unsigned long)fs.texConv, (unsigned long)fs.texEvict, (unsigned long)fs.texKB);
    }
    g_frameLogCount = 0;
  }
  if (g_frames <= 5 || (g_frames <= 300 && g_frames % 10 == 0) || g_frames % 60 == 0)   // (every 10th early frame: where a console run stops)
    n3dsLog("[3ds] frame %llu: %u/%u draws (gx %u) copies %u/%u  process %.0f ms (%u calls) render %.0f ms (build %.0f plan %.0f draw %.0f) present %.1f ms fence %d thread %d snapshot %lu KB fallback %lu verify-miss %lu\n", (unsigned long long)g_frames.load(), unsigned(F.drawn), unsigned(F.draws),
            unsigned(F.gxDraws), unsigned(F.copiesDone), unsigned(F.copies), processMs, processCalls, renderMs, F.buildMs, F.planMs, F.drawMs, presentMs, int(g_measurePresent), n3dsRenderThreadActive(), (unsigned long)(snapshotBytes>>10), (unsigned long)g_snapshotFallbacks, (unsigned long)g_mem.verifyMissing);
  static uint32_t lastKicks=0,lastSplits=0;const uint32_t kicksNow=g_r->backend().stats().kicks,splitsNow=g_r->backend().stats().splits;
  if(queueSample&&(g_perf.modelMaterialIds||sPreparedMaterialsEnabled))n3dsLog("[model-id] hits %llu misses %llu audit-failures %llu prepared %llu\n",(unsigned long long)sMaterialPrograms.versionHits.load(std::memory_order_relaxed),(unsigned long long)sMaterialPrograms.versionMisses.load(std::memory_order_relaxed),(unsigned long long)sMaterialPrograms.versionAuditFailures.load(std::memory_order_relaxed),(unsigned long long)sMaterialPrograms.preparedCalls.load(std::memory_order_relaxed));
  if(queueSample&&frame&&g_perf.stateTemplates)n3dsLog("[state-template] hits %u misses %u\n",frame->templateHits,frame->templateMisses);
  if(queueSample)n3dsLog("[queue-cost] frame %llu begin-wait %.2f submit %.2f final-fence %.2f render-total %.2f process %.2f ms gpu-tail %.2f ms; since last: gpu kicks %u splits %u\n",
    (unsigned long long)g_frames.load(),double(F.beginWaitTicks)*1000.0/SYSCLOCK_ARM11,double(F.endSubmitTicks)*1000.0/SYSCLOCK_ARM11,double(finishWaitTicks)*1000.0/SYSCLOCK_ARM11,renderMs,processMs,double(C3D_GetDrawingTime()),kicksNow-lastKicks,splitsNow-lastSplits);
  if(queueSample){lastKicks=kicksNow;lastSplits=splitsNow;}
  if(queueSample){const double k=1000.0/SYSCLOCK_ARM11;   // (Claude v69) where render-total goes outside build/plan/draw
    n3dsLog("[render-split] frame %llu policy %.2f cache-begin %.2f loop %.2f (draws %.2f) copies %.2f end+recycle %.2f matrix-prep %.2f event-recycle %.2f ms\n",(unsigned long long)g_frames.load(),
      F.splitTicks[0]*k,F.splitTicks[1]*k,F.splitTicks[2]*k,F.buildMs+F.planMs+F.drawMs,F.splitTicks[3]*k,F.splitTicks[4]*k,F.splitTicks[5]*k,F.splitTicks[6]*k);
    // (Claude v86) matrix-prep = key+lookup / appends / variant+uniforms (minus the tick-pair calibration), and event-recycle's event destruction part
    static uint64_t s_dcMiss=0,s_dcEvict=0,s_dcLook=0;const t2p::DrawCacheStats dc=g_r->drawCache().stats();   // (Claude v105) compile misses per frame
    n3dsLog("[render-split2] frame %llu matrix key %.2f append %.2f variant %.2f calib %.2f groups %llu members %llu; recycle pop %.2f ms; drawcache lookups %llu misses %llu evictions %llu entries %zu\n",(unsigned long long)g_frames.load(),
      F.matrixSplit[0]*k,F.matrixSplit[1]*k,F.matrixSplit[2]*k,F.matrixSplit[3]*k,(unsigned long long)F.matrixSplit[4],(unsigned long long)F.matrixSplit[5],F.splitTicks[7]*k,
      (unsigned long long)(dc.lookups-s_dcLook),(unsigned long long)(dc.misses-s_dcMiss),(unsigned long long)(dc.evictions-s_dcEvict),dc.entries);
    s_dcMiss=dc.misses;s_dcEvict=dc.evictions;s_dcLook=dc.lookups;
    {static uint64_t s_2d=0; n3dsLog("[draws2d] frame %llu orthographic draws %llu hash %016llx\n",(unsigned long long)g_frames.load(),(unsigned long long)(g_n3ds2dDraws-s_2d),(unsigned long long)g_n3ds2dHash); s_2d=g_n3ds2dDraws; g_n3ds2dHash=0;}
    {static uint64_t s_ind=0; if(g_n3dsIndirectDraws!=s_ind){n3dsLog("[indirect] frame %llu draws with a dropped indirect warp: %llu\n",(unsigned long long)g_frames.load(),(unsigned long long)(g_n3dsIndirectDraws-s_ind)); s_ind=g_n3dsIndirectDraws;}}
    if(t2p::g_compileClock&&dc.misses){const uint64_t* P=t2p::g_compilePhase;n3dsLog("[compile-phase] total ms: vs1 %.1f vs2 %.1f parse %.1f tev %.1f fixed %.1f (fog %.1f) all %.1f (%llu misses)\n",P[0]*k,P[1]*k,P[2]*k,P[3]*k,P[4]*k,P[6]*k,P[5]*k,(unsigned long long)dc.misses);}
    {auto& up=t2p::UniformBlockPool::get();auto& ep=t2p::EventNodePool::get();
     n3dsLog("[pools] frame %llu uniforms hits %llu misses %llu kept %u (%lu KB); events hits %llu misses %llu kept %u (%lu KB)\n",(unsigned long long)g_frames.load(),up.hits,up.misses,up.count,(unsigned long)((up.count*up.size)>>10),
             ep.hits,ep.misses,ep.count,(unsigned long)((ep.count*ep.size)>>10));}
    n3dsLockStatsLog((unsigned long)g_frames.load());n3dsAllocSitesLog((unsigned long)g_frames.load());n3dsFastAllocLog();
    if(g_perf.uiMapBottom)n3dsLog("[ui-route] frame %llu markers %u routed %u refused %u copies %u switches %u owns %d\n",(unsigned long long)g_frames.load(),F.routeMarkers,F.routedDraws,F.routeRefused,F.routeCopies,F.routeSwitches,int(g_routeOwnsBottom.load()));
    if(F.planDeltas||F.planDeltaFails||F.planDeltaChecks)n3dsLog("[plan-delta] frame %llu deltas %u fails %u checks %u mismatches %u\n",(unsigned long long)g_frames.load(),F.planDeltas,F.planDeltaFails,F.planDeltaChecks,F.planDeltaMismatches);
    n3dsLog("[resident] frame %llu hits %u misses %u inserts %u full %u entries %lu (%lu KB)\n",(unsigned long long)g_frames.load(),F.residentHits,F.residentMisses,F.residentInserts,F.residentFull,(unsigned long)F.residentEntries,(unsigned long)(F.residentBytes>>10));
    if(g_r->drawReplay()){
      {   // (Claude v120 DIAG) env N3DS_REPLAY_PROF=1: what a replay hit costs on the console, per step (bench RPROF line)
        static const bool rp=[]{const char* v=std::getenv("N3DS_REPLAY_PROF");return v&&*v=='1';}();
        t2p::g_replayProfOn=rp;
        if(rp){const uint64_t* q=t2p::g_replayProf;const double h=q[5]?double(q[5]):1.0,us=1e6/SYSCLOCK_ARM11;
          n3dsLog("[replay-prof] %.0f hits, us/hit: lookup range %.1f tail %.1f drawkey %.1f, remap %.1f textures %.1f repack %.1f backend %.1f [shader+attr %.1f uniforms %.1f texenv+tex %.1f effect %.1f updatectx %.1f drawcall %.1f]\n",
            double(q[5]),q[12]*us/h,q[13]*us/h,q[14]*us/h,q[1]*us/h,q[2]*us/h,q[3]*us/h,q[4]*us/h,q[6]*us/h,q[7]*us/h,q[8]*us/h,q[9]*us/h,q[10]*us/h,q[11]*us/h);
          for(int i=0;i<16;i++)t2p::g_replayProf[i]=0;}
      }
      if(g_perf.directEvents||g_perf.modelDirect)n3dsLog("[direct] draws %u fallback %u groups %u\n",F.directDraws,F.directFallbacks,F.directGroups);
      n3dsLog("[replay] frame %llu kb %u hits %u (recorded %u, new projection %u) misses %u stale %u inserts %u groups %u/%u; misses so far: no-entry %llu tail %llu draw-state %llu 2d-uniforms %llu\n",(unsigned long long)g_frames.load(),F.replayKb,F.replayHits,F.replayRecorded,F.replayProj,F.replayMisses,F.replayStale,F.replayInserts,F.replayGroupHits,F.replayGroupHits+F.replayGroupMisses,(unsigned long long)t2p::g_replayWhy[0],(unsigned long long)t2p::g_replayWhy[1],(unsigned long long)t2p::g_replayWhy[2],(unsigned long long)t2p::g_replayWhy[3]);}
    n3dsLog("[draw-reuse] frame %llu hits %u misses %u inserts %u full %u entries %lu (%lu KB) arena peak %lu KB linear free %lu KB textures %lu KB\n",(unsigned long long)g_frames.load(),
      F.reuseHits,F.reuseMisses,F.reuseInserts,F.reuseFull,(unsigned long)F.reuseEntries,(unsigned long)(F.reuseBytes>>10),(unsigned long)(g_r->backend().stats().arenaPeak>>10),
      (unsigned long)(linearSpaceFree()>>10),(unsigned long)(g_r->backend().stats().textureBytes>>10));}
  if (!g_noBundleWrites && g_frames % 120 == 0 && g_r->missEntries() != g_missesWritten) {   // materials the bundles lacked: loaded next time, and input for the offline translator
    g_missesWritten = g_r->missEntries();
    const std::vector<uint8_t> m = g_r->missBundle();
    if (FILE* f = std::fopen(N3DS_ROOT "/ww_bundle_misses.tmp", "wb")) {   // written whole, then renamed: a power-off mid-write keeps the old file
      const bool ok = std::fwrite(m.data(), 1, m.size(), f) == m.size();
      std::fclose(f);
      if (ok) { std::remove(N3DS_ROOT "/ww_bundle_misses.bin"); std::rename(N3DS_ROOT "/ww_bundle_misses.tmp", N3DS_ROOT "/ww_bundle_misses.bin"); }
    }
  }
  if(((g_perf.bottomUi&&g_bottomUi.takeDirty())||g_consoleOn) && !g_routeOwnsBottom.load(std::memory_order_acquire)) {   // (Claude v80) citro3d presents the bottom screen while the route owns it
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
  }
  g_bottomUi.capture((unsigned long)g_frames.load());
}
int n3dsGpuInit(void) {
  std::atexit(n3dsGpuShutdown);
  bool recoveredPrefs=false;FILE* prefs=std::fopen(N3DS_ROOT "/performance.ini","r");
  if(!prefs&&errno==ENOENT){prefs=std::fopen(N3DS_ROOT "/performance.ini.ui.bak","r");if(prefs){recoveredPrefs=true;n3dsLog("[settings] recovered preferences from backup\n");}}
  if(prefs){g_perf.read(prefs);std::fclose(prefs);if(recoveredPrefs)std::rename(N3DS_ROOT "/performance.ini.ui.bak",N3DS_ROOT "/performance.ini");}
  else if(errno==ENOENT){if(FILE* f=std::fopen(N3DS_ROOT "/performance.ini","w")){std::fputs(Performance3ds::example(),f);std::fclose(f);}}
  if(!g_perf.widescreen){g_perf.topHud=false;if(n3dsUiSetTopHudEnabled)n3dsUiSetTopHudEnabled(0);}
  citro_settings::fpsEnabled.store(g_perf.frameCounter,std::memory_order_relaxed);
  { const char* o=std::getenv("N3DS_PROF_ORIGIN"); g_gxOriginProf = o && o[0]=='1'; }
  t2p::g_decodedInitMode=g_perf.decodeOverwrite?1:0; // publish before worker startup
  const bool debugConsole=std::getenv("N3DS_DEBUG_CONSOLE")!=nullptr;
  if(debugConsole)g_perf.bottomUi=false;
  if(!debugConsole){g_consoleOn=false;g_bottomUi.init();
    if(!g_perf.bottomUi){gfxFlushBuffers();gfxScreenSwapBuffers(GFX_BOTTOM,false);g_bottomUi.init();gfxFlushBuffers();gfxScreenSwapBuffers(GFX_BOTTOM,false);}
    // (Claude v103) the game's own printf/OSReport (e.g. d_save.cpp "SAVE size:%d" / "LOAD size:%d") went to the libctru console that boot
    // opened on the bottom screen and drew text over the panel: stdout/stderr now go to log.txt only.
    static const devoptab_t toLog={"n3dslog",0,nullptr,nullptr,
      [](struct _reent*,void*,const char* p,size_t n)->ssize_t{ char b[256]; size_t k=n<sizeof b-1?n:sizeof b-1; std::memcpy(b,p,k); b[k]=0; n3dsLog("%s",b); return ssize_t(n); },
      nullptr,nullptr,nullptr};
    devoptab_list[STD_OUT]=&toLog; devoptab_list[STD_ERR]=&toLog; setvbuf(stdout,nullptr,_IONBF,0); setvbuf(stderr,nullptr,_IONBF,0);}
  if(g_perf.core2>=0){setenv("N3DS_RENDER_THREAD",g_perf.core2?"1":"0",1);setenv("N3DS_RENDER_PIPELINE",g_perf.core2?"1":"0",1);}
  n3dsLog("[perf] fidelity-preserving settings: native 400x240 core2 %d material_cache %d geometry_cache %d vertex_runs %d packed_uniforms %d plan_reuse %d fog_prepare %d vertex_plan %d vertex_decode %d palette_cache %d bottom_ui %d draw_timing %d lean_state %d retain_geometry %d state_inplace %d pipeline_stage8 %d heap_fence %d fifo_thread %d fifo_core %d decode_helper %d ignored %u\n",g_perf.core2,int(g_perf.materialCache),int(g_perf.geometryCache),int(g_perf.vertexRuns),int(g_perf.packedUniforms),int(g_perf.planReuse),int(g_perf.fogPreparation),int(g_perf.vertexPlan),int(g_perf.vertexDecode),int(g_perf.paletteCache),int(g_perf.bottomUi),int(g_perf.drawTiming),int(g_perf.leanState),int(g_perf.retainGeometry),int(g_perf.stateInPlace),int(g_perf.pipelineStage8),int(g_perf.heapFence),int(g_perf.fifoThread),g_perf.fifoCore,int(g_perf.decodeHelper),g_perf.errors);
  n3dsLog("[perf] j3d_geometry %d ignored %u\n",int(g_perf.j3dGeometry),g_perf.errors);
  n3dsLog("[perf] j3d_programs %d uniform_sections %d foliage_far %d pool_floor %d batch_fx %d frame_skip %d paint_pacing %d paint_max_gap %d paint_cadence %d skip_draw %d audio_core1 %d real_shadows %d early_release %d ignored %u\n", int(g_perf.j3dPrograms),int(g_perf.uniformSections),g_perf.foliageFar,g_perf.poolFloor,int(g_perf.batchFx),g_perf.frameSkip,g_perf.paintPacing,g_perf.paintMaxGap,g_perf.paintCadence,int(g_perf.skipDraw),int(g_perf.audioCore1),int(g_perf.realShadows),int(g_perf.earlyRelease),g_perf.errors);
  n3dsLog("[perf] extended settings: typed_gx %d direct_texture %d state_pool %d vertex_pool_fifo %d plan_notes %d direct_pack %d decode_overwrite %d matrix_batch %d plan_templates %d packet_worker %d packet_worker_verify %d state_incremental %d ignored %u\n",int(g_perf.typedGx),int(g_perf.directTexture),int(g_perf.statePool),int(g_perf.vertexPoolFifo),int(g_perf.planNotes),int(g_perf.directPack),int(g_perf.decodeOverwrite),int(g_perf.matrixBatch),int(g_perf.planTemplates),int(g_perf.packetWorker),int(g_perf.packetWorkerVerify),int(g_perf.stateIncremental),g_perf.errors);
  const char* pipeline=std::getenv("N3DS_RENDER_PIPELINE");
  g_pipeline=pipeline && !std::strcmp(pipeline,"1");
  if (n3dsRenderThreadStart()) {
    const int ok=n3dsRenderThreadInit();
    if (ok && g_pipeline) {
      g_cpuFrames[0]=new t2p::CpuFrame3ds; g_cpuFrames[1]=new t2p::CpuFrame3ds;
      for(auto* frame:g_cpuFrames) frame->snapshot.init(2u<<20);
      g_recorder=new t2p::FrameRecorder3ds;
      if(g_perf.directEvents||g_perf.modelDirect)g_recorder->setDirectRegistry(g_r->directRegistry());
      g_recorder->setDirectSnapshots(g_perf.directSnapshot);
      g_recorder->setDirectDiagnostics(std::getenv("N3DS_DIRECT_DIAG")!=nullptr);
      if(g_perf.modelDirect)g_recorder->setGroupRegistry(g_r->groupRegistry());
      if(g_perf.preparedShapes)n3dsLog("[prepared] shape programs loaded %d\n",int(g_recorder->fifo().loadPreparedShapes(N3DS_ROOT "/ww_shapes.bin")));
      g_recorder->fifo().setPrimitiveRuns(g_perf.vertexRuns);
      g_recorder->setStateNotes(!g_perf.leanState);
      g_recorder->setStateInPlace(g_perf.stateInPlace);
      g_recorder->setStatePool(g_perf.statePool);
      g_recorder->setStateIncremental(g_perf.stateIncremental);
      g_recorder->setStateTemplates(g_perf.stateTemplates);n3dsLog("[state] templates %d\n",int(g_perf.stateTemplates));
      g_recorder->setRetainGeometry(g_perf.retainGeometry,size_t(g_perf.retainKb)<<10);n3dsLog("[perf] retain_geometry %d budget %d KB (recorder)\n",int(g_perf.retainGeometry),g_perf.retainKb);
      g_recorder->setVertexPoolFifo(g_perf.vertexPoolFifo);
      g_recorder->setCompactDrawStorage(g_perf.recordCompact);n3dsLog("[perf] record_compact %d tex_error_table %d\n",int(g_perf.recordCompact),int(g_perf.texErrorTable));
      g_recorder->setHeapProbe(g_perf.heapFence?heapRoom:nullptr);
      if (const char* c = std::getenv("N3DS_HEAP_TEST_CEILING_MB")) { const int mb = atoi(c); if (mb >= 16 && mb <= 85) g_heapCeiling = size_t(mb) << 20; }
      g_recorder->setHeapReady(g_perf.heapFence && g_perf.stagingPartial ? heapReady : nullptr);
      n3dsLog("[perf] staging_partial %d heap ceiling %u MB\n", int(g_perf.stagingPartial), unsigned(g_heapCeiling >> 20));
      g_recorder->fifo().setGeometryCache(g_perf.geometryCache);
      g_recorder->fifo().setFastAttributeDecode(g_perf.vertexDecode);
      g_recorder->fifo().setCachedVertexPlan(g_perf.vertexPlan);
      g_recorder->fifo().setAuroraMode(true);
      g_recorder->fifo().setUncheckedArrays(true);
      g_recorder->fifo().setStopAtDrawDone(true);
      startDecodeHelper(g_recorder->fifo());
      g_recorder->fifo().setGuestArrayReader([](uint32_t a,uint32_t){return reinterpret_cast<const uint8_t*>(uintptr_t(a));});
      g_recorder->select(g_cpuFrames[0]);
      n3dsLog("[render-thread] immutable frame pipeline enabled\n");
    }
    return ok;
  }
  g_pipeline=false;
  return n3dsGpuInitLocal();
}
// (Claude) N3DS_PROF_ORIGIN=1 diagnostic (inline FIFO only): GXCallDisplayList is wrapped (origin_prof_3ds.cpp)
// to process the stream up to and after each raw display list, so parse time, bytes, draws and vertices split
// between the game's typed GX calls (origin 0) and raw display lists (origin 1).
extern "C" int n3dsOriginProfOn(void) { return g_gxOriginProf ? 1 : 0; }
extern "C" void n3dsSetGxOrigin(int o) { g_gxOrigin = o ? 1 : 0; }
uint32_t n3dsGpuProcess(const uint8_t* data,uint32_t size,int* done) {
  if (g_pipeline) {
    auto& frame=*g_recorder->current;
    const u64 t0=svcGetSystemTick();
    ++frame.processCalls;
    const uint64_t d0=g_recorder->fifo().drawCount(), v0=g_t2pProf[2];
    g_n3dsMark[2]=g_n3dsMark[2]+1;
    g_recorder->inProcess=true;
    size_t used=g_recorder->fifo().process(data,size);
    g_recorder->inProcess=false;
    g_n3dsMark[3]=g_n3dsMark[3]+1;
    frame.processTicks+=svcGetSystemTick()-t0;
    if(g_gxOriginProf){const int o=g_gxOrigin;g_originTicks[o]+=svcGetSystemTick()-t0;g_originBytes[o]+=used;g_originDraws[o]+=g_recorder->fifo().drawCount()-d0;g_originVerts[o]+=g_t2pProf[2]-v0;g_originCalls[o]++;}
    *done=g_recorder->fifo().takeDrawDone()?1:0;
    if(used==0 || (used<size && !*done))used=size;
    return uint32_t(used);
  }
  if (n3dsRenderThreadActive()) return n3dsRenderThreadProcess(data,size,done);
  return n3dsGpuProcessLocal(data,size,done);
}
static int g_abFoliageFar = [] { const char* v = std::getenv("N3DS_AB_FOLIAGE"); return v ? std::atoi(v) : 3000; }();   // (Claude v100)
extern "C" void n3dsEndFrameRelease(void);
extern "C" void n3dsPacingNoteProcess(uint64_t ticks);
void n3dsGpuEndFrame(void) {
  {   // (Claude v95) env N3DS_AB_SHAPES=1: console A/B in one session, j3d_geometry (shape references) flips every 300 frames; "[ab]" lines mark it
    static const bool ab = std::getenv("N3DS_AB_SHAPES") != nullptr;
    if (ab && g_perf.j3dGeometry != (((g_frames.load() / 300) & 1) == 0)) { g_perf.j3dGeometry = !g_perf.j3dGeometry; n3dsLog("[ab] frame %llu: shape references %d\n", (unsigned long long)g_frames.load(), int(g_perf.j3dGeometry)); }
  }
  {   // (Claude v99) env N3DS_AB_CYCLE=1: console A/B in one session, 300-frame phases alternating: 0 = draw_reuse off, 1 = on; "[ab]" lines
    static const bool cyc = std::getenv("N3DS_AB_CYCLE") != nullptr;
    static int lastPhase = -1;
    if (cyc) {
      const int ph = int((g_frames.load() / 300) % 2);
      if (ph != lastPhase) {
        lastPhase = ph;
        g_foliageAb.store(ph == 1 ? g_abFoliageFar : 0);   // (Claude v100) foliage_far off / N3DS_AB_FOLIAGE (default 3000)
        n3dsLog("[ab] frame %llu: phase %d foliage_far=%d\n", (unsigned long long)g_frames.load(), ph, ph ? g_abFoliageFar : 0);
      }
    }
  }
  static const bool coreProbe = [] { const char* v = std::getenv("N3DS_CORE_PROBE"); return v && *v == '1'; }();   // (Claude v120) opt-in: ~320 ms hitch on every core
  if (coreProbe && (g_frames.load() == 600 || g_frames.load() == 1500)) coreprobe::run(g_frames.load() == 600 ? "frame600" : "frame1500");   // (Claude v94) same probe while the game runs
  if(g_perf.j3dGeometry && g_recorder && (g_frames.load()%120)==0){static unsigned long long lastRefs=0;static unsigned lastGen=0;const unsigned long long refs=g_recorder->fifo().shapeRefs();const unsigned gen=g_n3dsShapeGen;
    n3dsLog("[j3d-shape] 120 frames: shape references %llu, model generation +%u\n",refs-lastRefs,gen-lastGen);lastRefs=refs;lastGen=gen;}   // (Claude v95)
  if(g_perf.preparedShapes && g_recorder && (g_frames.load()%120)==0)n3dsLog("[prepared-shape] hits %llu misses %llu runs %llu bytes %u\n",(unsigned long long)g_recorder->fifo().preparedShapeHits(),(unsigned long long)g_recorder->fifo().preparedShapeMisses(),(unsigned long long)g_recorder->fifo().preparedShapeRuns(),unsigned(g_recorder->fifo().preparedShapeBytes()));
  if((g_perf.modelRecords||g_perf.materialRecords) && g_recorder && (g_frames.load()%120)==0)n3dsLog("[j3d-model] groups %llu materials %llu\n",(unsigned long long)g_recorder->fifo().modelGroups(),(unsigned long long)g_recorder->fifo().materialRefs());
  if(g_perf.j3dPrograms && g_recorder && (g_frames.load()%120)==0) n3dsLog("[j3d-program] hits %llu misses %llu\n", (unsigned long long)g_recorder->fifo().stateProgramHits(), (unsigned long long)g_recorder->fifo().stateProgramMisses());
  if (g_pipeline) {
    g_n3dsMark[4]=1;
    const uint64_t ej0=svcGetSystemTick();   // (Claude v99) end-of-frame job split: [endjob] lines
    finishRecording(g_recorder->fifo());
    const uint64_t ej1=svcGetSystemTick();
    g_n3dsMark[4]=2;
    if(g_perf.retainGeometry && (g_inputFrames.load()+1)%60==0){
      const t2p::GxRetainStats rs=g_recorder->takeRetainStats();
      n3dsLog("[retain] 60 frames: draws %llu skipped %llu (%.0f%%), batches %llu retained %llu published %llu materialized %llu not-retainable %llu over-budget %llu; compared %llu KB arrays + %llu KB stream, snapshots %llu KB, held %zu KB; shape-reference matches %llu of %llu offered (new batch: no key %llu, failed %llu; mid-batch miss %llu)\n",
        (unsigned long long)rs.draws,(unsigned long long)rs.skipped,rs.draws?100.0*double(rs.skipped)/double(rs.draws):0.0,(unsigned long long)rs.batches,(unsigned long long)rs.retainedBatches,
        (unsigned long long)rs.published,(unsigned long long)rs.materialized,(unsigned long long)rs.notRetainable,(unsigned long long)rs.overBudget,
        (unsigned long long)(rs.verifyBytes>>10),(unsigned long long)(rs.streamBytes>>10),(unsigned long long)(rs.snapshotBytes>>10),rs.heldBytes>>10,(unsigned long long)rs.shapeMatches,(unsigned long long)rs.shapeOffered,(unsigned long long)rs.shapeNewNoPrev,(unsigned long long)rs.shapeNewMiss,(unsigned long long)rs.shapeMidMiss);
      n3dsLog("[retain-miss] 60 frames, batches/kverts decoded by reason: not-retainable %llu/%llu no-key %llu/%llu header %llu/%llu bindings %llu/%llu stream %llu/%llu array-bytes %llu/%llu shape %llu/%llu mid-batch %llu/%llu over-budget %llu/%llu\n",
        (unsigned long long)rs.missBatches[0],(unsigned long long)(rs.missVerts[0]/1000),(unsigned long long)rs.missBatches[1],(unsigned long long)(rs.missVerts[1]/1000),(unsigned long long)rs.missBatches[2],(unsigned long long)(rs.missVerts[2]/1000),(unsigned long long)rs.missBatches[3],(unsigned long long)(rs.missVerts[3]/1000),(unsigned long long)rs.missBatches[4],(unsigned long long)(rs.missVerts[4]/1000),(unsigned long long)rs.missBatches[5],(unsigned long long)(rs.missVerts[5]/1000),(unsigned long long)rs.missBatches[6],(unsigned long long)(rs.missVerts[6]/1000),(unsigned long long)rs.missBatches[7],(unsigned long long)(rs.missVerts[7]/1000),(unsigned long long)rs.missBatches[8],(unsigned long long)(rs.missVerts[8]/1000));   // (Claude v97 diag)
    }
    auto* frame=g_recorder->current;
    if ((g_inputFrames.load()+1)%60==0) n3dsGameHeapReport(frame->heaps,sizeof frame->heaps);
    const uint64_t ej2=svcGetSystemTick();
    frame->frozen=frame->snapshot.freeze(readableTexture,snapshotSkip);
    {   // (Claude v116 DIAG) env N3DS_SNAP_LOG=1: every 120 frames, the copied (not cache-known) texture ranges
      static int on=-1, n=0; if(on<0){const char* v=getenv("N3DS_SNAP_LOG"); on=(v&&*v=='1')?1:0;}
      if(on && frame->frozen && (++n%120)==0){
        std::string line; char b[48]; int k=0;
        frame->snapshot.forEachCopied([&](uint32_t a,uint32_t sz){ if(sz>=2048 && k<40){ snprintf(b,sizeof b," %08lx+%lu",(unsigned long)a,(unsigned long)sz); line+=b; k++; } });
        n3dsLog("[snap] copied %u KB:%s\n",unsigned(frame->snapshot.bytes()>>10),line.c_str());
      }
    }
    n3dsEndFrameRelease();   // (Claude v115) the game may reuse its memory now (aurora_3ds.cpp)
    {   // (Claude v121) replay keys on this core (it has slack; the render core is the bottleneck): renderer3ds.cpp computeReplayKey
      static const bool rkOn=[]{const char* v=std::getenv("N3DS_REPLAY_KEYS");return v&&*v=='1';}();   // default off: the worker cost outweighs the render-core saving (bench +6.6 / -1.9 ms)
      if(rkOn && g_r && g_r->drawReplay()) for(auto& ev:frame->events) if(ev.kind==t2p::RecordedEvent3ds::Draw && ev.geo) t2p::computeReplayKey(ev);
    }
    const uint64_t ej3=svcGetSystemTick();
    for(int i=0;i<16;++i) {frame->prof[i]=g_t2pProf[i];g_t2pProf[i]=0;}
    for(int i=0;i<8;++i) {g_recStateProf[i]=g_t2pStateProf[i];g_t2pStateProf[i]=0;}   // (Claude v100) [prof-state]: the last recorded frame's sections
    const uint64_t handoffStart=svcGetSystemTick();
    n3dsGpuSync(); // Previous buffer is returned before its next use.
    frame->handoffWaitTicks=svcGetSystemTick()-handoffStart;
    g_n3dsMark[4]=3;
    g_submitted=frame;
    n3dsPacingNoteProcess(frame->processTicks);
    g_submittedSlot = n3dsPaintSlotOf(g_paintHandedCount());   // (Claude v120) the paint cadence follows the decode time too
    n3dsRenderThreadFrame(); // Acknowledges after snapshot import, before replay.
    {
      static uint64_t acc[5]; static unsigned n=0; const uint64_t ej4=svcGetSystemTick();
      acc[0]+=ej1-ej0; acc[1]+=ej2-ej1; acc[2]+=ej3-ej2; acc[3]+=handoffStart?0:0; acc[4]+=ej4-ej3;
      if(++n==60){ const double k=1000.0/SYSCLOCK_ARM11/60;
        n3dsLog("[endjob] per frame: finish-recording %.2f stats+heaps %.2f snapshot-freeze %.2f (%u KB, skipped %u KB) handoff+ack %.2f ms\n",acc[0]*k,acc[1]*k,acc[2]*k,unsigned(frame->snapshot.bytes()>>10),unsigned(frame->snapshot.skippedBytes()>>10),acc[4]*k);
        for(auto& a:acc)a=0; n=0; }
    }
    g_n3dsMark[4]=0;
    g_recordIndex^=1;
    auto* next=g_cpuFrames[g_recordIndex];
    next->processTicks=0;next->processCalls=0;next->heaps[0]=0;
    g_recorder->select(next);
  }
  else if (n3dsRenderThreadActive()) n3dsRenderThreadFrame();
  else n3dsGpuEndFrameLocal();
  ++g_inputFrames;
  n3dsPgoTick(unsigned(g_inputFrames));   // (Claude v111) PGO dump (instrumented builds only)
}

// (Claude v111) minimal LLVM IR-PGO runtime for the instrumented game build (build.sh N3DS_PGO=gen): no compiler-rt profile library exists for this
// target, so the raw profile (format version 10, 32-bit) is written here from the __llvm_prf_* sections. Env N3DS_PGO_DUMP=<frame> writes
// N3DS_ROOT/ww.profraw once at that frame. In normal builds the sections are absent and nothing happens.
extern "C" {
int __llvm_profile_runtime = 0;
extern uint64_t __llvm_profile_raw_version __attribute__((weak));
extern char __start___llvm_prf_data[] __attribute__((weak)), __stop___llvm_prf_data[] __attribute__((weak));
extern char __start___llvm_prf_cnts[] __attribute__((weak)), __stop___llvm_prf_cnts[] __attribute__((weak));
extern char __start___llvm_prf_names[] __attribute__((weak)), __stop___llvm_prf_names[] __attribute__((weak));
extern char __start___llvm_prf_bits[] __attribute__((weak)), __stop___llvm_prf_bits[] __attribute__((weak));
}
static void n3dsPgoTick(unsigned frame) {
  static const unsigned dumpAt = [] { const char* v = std::getenv("N3DS_PGO_DUMP"); return v ? unsigned(std::strtoul(v, nullptr, 10)) : 0u; }();
  static bool done = false;
  if (!dumpAt || done || frame < dumpAt) return;
  done = true;
  if (!&__llvm_profile_raw_version || !__start___llvm_prf_data) { n3dsLog("[pgo] not an instrumented build\n"); return; }
  const uintptr_t db = uintptr_t(__start___llvm_prf_data), de = uintptr_t(__stop___llvm_prf_data);
  const uintptr_t cb = uintptr_t(__start___llvm_prf_cnts), ce = uintptr_t(__stop___llvm_prf_cnts);
  const uintptr_t nb = uintptr_t(__start___llvm_prf_names), ne = uintptr_t(__stop___llvm_prf_names);
  const uintptr_t bb = __start___llvm_prf_bits ? uintptr_t(__start___llvm_prf_bits) : 0, be = __start___llvm_prf_bits ? uintptr_t(__stop___llvm_prf_bits) : 0;
  constexpr size_t kDataSize = 48;   // __llvm_profile_data on a 32-bit target (InstrProfData.inc, LLVM 19)
  auto pad8 = [](uint64_t n) { return (8 - (n & 7)) & 7; };
  const uint64_t dataBytes = de - db, cntBytes = ce - cb, nameBytes = ne - nb, bitBytes = be - bb;
  const uint64_t magic = (uint64_t)255 << 56 | (uint64_t)'l' << 48 | (uint64_t)'p' << 40 | (uint64_t)'r' << 32 | (uint64_t)'o' << 24 | (uint64_t)'f' << 16 | (uint64_t)'R' << 8 | (uint64_t)129;
  const uint64_t hdr[16] = {magic, __llvm_profile_raw_version, 0, dataBytes / kDataSize, pad8(dataBytes), cntBytes / 8, pad8(cntBytes), bitBytes, pad8(bitBytes),
                            nameBytes, uint64_t(cb - db), uint64_t(bb ? bb - db : 0 - db), uint64_t(nb), 0, 0, 2 /*IPVK_Last*/};
  FILE* f = std::fopen(N3DS_ROOT "/ww.profraw", "wb");
  if (!f) { n3dsLog("[pgo] cannot write ww.profraw\n"); return; }
  static const uint8_t zeros[8] = {};
  std::fwrite(hdr, sizeof hdr, 1, f);
  std::fwrite(reinterpret_cast<const void*>(db), 1, dataBytes, f); std::fwrite(zeros, 1, pad8(dataBytes), f);
  std::fwrite(reinterpret_cast<const void*>(cb), 1, cntBytes, f); std::fwrite(zeros, 1, pad8(cntBytes), f);
  if (bitBytes) std::fwrite(reinterpret_cast<const void*>(bb), 1, bitBytes, f);
  std::fwrite(zeros, 1, pad8(bitBytes), f);
  std::fwrite(reinterpret_cast<const void*>(nb), 1, nameBytes, f); std::fwrite(zeros, 1, pad8(nameBytes), f);
  std::fclose(f);
  n3dsLog("[pgo] wrote ww.profraw at frame %u: %llu functions, %llu counters, names %llu bytes\n", frame, (unsigned long long)(dataBytes / kDataSize),
          (unsigned long long)(cntBytes / 8), (unsigned long long)nameBytes);
}
void n3dsGpuShutdownLocal(void) {
  if (!g_r) return;
  C3Di_RenderQueueWaitDone();
  if(g_processExiting.load()){
    // GPU is idle, but the optional decode helper can still reference recorder
    // buffers. Keep CPU objects alive until the kernel terminates all threads.
    g_r->stopPacketWorker();C3D_Fini();return;
  }
  delete g_r; g_r=nullptr;
  if (g_tgt) { C3D_RenderTargetDelete(g_tgt); g_tgt=nullptr; }
  if (g_shots) { std::fclose(g_shots); g_shots=nullptr; }
  C3D_Fini();
  delete g_recorder;g_recorder=nullptr;
  for(auto*& frame:g_cpuFrames){delete frame;frame=nullptr;}
}

unsigned int n3dsCurrentThreadHandle(void) {
  Handle h = 0;
  if (R_FAILED(svcDuplicateHandle(&h, CUR_THREAD_HANDLE))) return 0;
  return h;
}

// GameCube priority 0 (highest) .. 31 -> 3DS 0x1C .. 0x3B; -1 (interrupt context) -> 0x1B.
// (Claude 2026-11-11) Was 0x18 / 0x19..: above libctru's GSP event thread (0x1A), which takes the GPU's "done" interrupts and drives citro3d's
// command queue, and level with its NDSP thread (0x18). The 3DS runs a thread only while nothing of a higher priority wants its core, so on the
// console the game's interrupt-context threads (DSP, AI: they start with the audio) could keep that thread out: the GPU queue stopped at the first
// frame after audio start (log-cia-run8: list submitted, never "completed"). Every game thread is now below both system threads; their order
// among themselves is unchanged.
void n3dsSetThreadPriority(unsigned int handle, int gcPriority) {
  const s32 p = gcPriority < 0 ? 0x1B : 0x1C + (gcPriority > 31 ? 31 : gcPriority);
  const Result r = svcSetThreadPriority(handle ? Handle(handle) : CUR_THREAD_HANDLE, p);
  static int logged = 0;
  if (logged < 40) { logged++; n3dsLog("[3ds] thread priority gc %d -> 0x%lx (%s, result %08lx)\n", gcPriority, (long)p, handle ? "other" : "self", (unsigned long)r); }
}

// pthread_create (newlib -> libctru's __syscall_thread_create) starts every thread at priority 0x3F, the lowest, so a new thread
// cannot run at all while a thread of the game busy-waits. Linked with --wrap: new threads start at their creator's priority
// (cos_sdk raises the creator to the new thread's GameCube priority around the call, n3dsPushCreatePriority).
struct __pthread_t;
// Core 1 (core1_threads, Claude): every thread made through pthread_create (cos_sdk OS threads: audio/DSP emulation, DVD/ARAM loading, alarms; the AI DMA
// and DSP interrupt threads) runs on the New 3DS's core 1, so the game thread has core 0 to itself (the renderer owner is on core 2). cos_sdk already runs
// these threads in parallel on the PC build (os_internal.h: one OS lock, real host threads), so this changes timing only, as more host cores do.
extern "C" int n3dsFifoThreadMode(void) {
  static const int on = [] {
    Performance3ds config;
    if (FILE* f = std::fopen(N3DS_ROOT "/performance.ini", "r")) { config.read(f); std::fclose(f); }
    n3dsLog("[threads] fifo_thread %d fifo_core %d\n", int(config.fifoThread), config.fifoCore);
    if (config.fifoThread && config.fifoCore != 1) {   // the worker's core must exist before the mode is chosen: on core 1 it is slower than inline
      s32 prio = 0x30;
      svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
      Thread probe = threadCreate([](void*) {}, nullptr, 0x1000, prio, config.fifoCore, false);
      if (!probe) { n3dsLog("[threads] core %d refuses threads: FIFO inline on the game thread\n", config.fifoCore); return 0; }
      threadJoin(probe, U64_MAX); threadFree(probe);
    }
    return config.fifoThread ? 1 : 0;
  }();
  return on;
}
// One-shot stack size for the next pthread (aurora_3ds.cpp sets it before Aurora creates its FIFO worker, a std::jthread with default attributes:
// the worker runs the recorder, which needs far more than the 32 KB default here).
static size_t g_nextThreadStack = 0;
extern "C" void n3dsNextThreadStack(size_t bytes) { g_nextThreadStack = bytes; }
static int threadCore() {
  static int core = [] {
    Performance3ds config;
    if (FILE* f = std::fopen(N3DS_ROOT "/performance.ini", "r")) { config.read(f); std::fclose(f); }
    if (!config.core1Threads) return 0;
    // (Claude v111) game threads go straight to a New 3DS core (bg_core, default 2). Core 1 (the system core, borrowed with
    // APT_SetAppCpuTimeLimit) only with bg_core=1: console v104-v110 kept one game thread there at exit, and every close from HOME ended in
    // "An error has occurred ... the system will now restart" (the exit path itself completed).
    if (config.bgCore >= 2) { n3dsLog("[threads] game threads on core %d (core 1 not borrowed)\n", config.bgCore); return config.bgCore; }
    if (config.bgCore == 0) { n3dsLog("[threads] game threads on core 0 (core 1 not borrowed)\n"); return 0; }
    const Result r = APT_SetAppCpuTimeLimit(80);
    n3dsLog("[threads] core1_threads 1: APT_SetAppCpuTimeLimit(80) %08lx\n", (unsigned long)r);
    return R_SUCCEEDED(r) ? 1 : 0;
  }();
  return core;
}
// (Claude) Console diagnosis: why the kernel refused a thread. The raw svcCreateThread result on the wanted core and on core 0 (a 4 KB
// scratch stack, a thread that returns at once), and the process's resource limits. Logged for the first few failures only.
static void threadFailureReport(s32 prio, int core) {
  static int reports = 0;
  if (reports >= 2) return;
  reports++;
  static u8 scratch[0x1000] __attribute__((aligned(8)));
  (void)core;
  for (int c : {0}) {   // (core 0 only: a refused attempt on another core costs a thread slot)
    Handle h = 0;
    const Result rc = svcCreateThread(&h, [](void*) { svcExitThread(); }, 0, reinterpret_cast<u32*>(scratch + sizeof scratch), prio, c);
    n3dsLog("[threads] diagnosis: svcCreateThread prio 0x%lx core %d -> %08lx\n", (long)prio, c, (unsigned long)rc);
    if (R_SUCCEEDED(rc)) { svcWaitSynchronization(h, 1000000000LL); svcCloseHandle(h); }
    if (c == 0) break;
  }
  Handle lim = 0;
  if (R_SUCCEEDED(svcGetResourceLimit(&lim, CUR_PROCESS_HANDLE))) {
    ResourceLimitType types[] = {RESLIMIT_PRIORITY, RESLIMIT_COMMIT, RESLIMIT_THREAD, RESLIMIT_EVENT, RESLIMIT_MUTEX, RESLIMIT_SEMAPHORE, RESLIMIT_TIMER, RESLIMIT_SHAREDMEMORY, RESLIMIT_ADDRESSARBITER, RESLIMIT_CPUTIME};
    s64 cur[10] = {}, max[10] = {};
    svcGetResourceLimitCurrentValues(cur, lim, types, 10);
    svcGetResourceLimitLimitValues(max, lim, types, 10);
    svcCloseHandle(lim);
    static const char* names[] = {"priority", "commit", "thread", "event", "mutex", "semaphore", "timer", "sharedmem", "arbiter", "cputime"};
    for (int i = 0; i < 10; i++) n3dsLog("[threads] diagnosis: limit %-9s %ld of %ld\n", names[i], (long)cur[i], (long)max[i]);
  }
  n3dsLog("[threads] diagnosis: application region free %lu KB\n", (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10));
}
// (Claude v114) one-shot request: the next thread goes to core 1 (the system core, borrowed with APT_SetAppCpuTimeLimit). Console logs (v102-v107)
// show core 1 takes ONE application thread (the first, at 0x30; later ones refused), so it is spent on the busiest background thread: the
// game's JAudio thread (JASAudioThread.cpp), which runs the sequence player and, through its DSP mails, the emulated DSP's audio rendering.
static std::atomic<bool> g_core1Next{false};
extern "C" void n3dsPreferCore1Next(void) {
  static const bool on = [] { Performance3ds c; if (FILE* f = std::fopen(N3DS_ROOT "/performance.ini", "r")) { c.read(f); std::fclose(f); } return c.audioCore1; }();
  if (on) g_core1Next.store(true);
}
int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stackAddr, size_t stackSize) {
  if (stackAddr) return EINVAL;
  s32 prio = 0x30;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  if (g_core1Next.exchange(false)) {
    static Result limit = APT_SetAppCpuTimeLimit(80);
    Thread t = R_SUCCEEDED(limit) ? threadCreate(reinterpret_cast<ThreadFunc>(func), arg, stackSize ? stackSize : 0x8000, prio < 0x30 ? 0x30 : prio, 1, false) : nullptr;
    Result raised = 0;
    if (t && prio < 0x30) raised = svcSetThreadPriority(threadGetHandle(t), prio);
    n3dsLog("[threads] audio thread -> core 1: time limit %08lx, create %s, priority 0x%lx raise %08lx\n", (unsigned long)limit, t ? "ok" : "REFUSED (normal placement)", (long)prio, (unsigned long)raised);
    if (t) { *thread = reinterpret_cast<struct __pthread_t*>(t); return 0; }
  }
  int core = threadCore();
  if (g_nextThreadStack) {   // the FIFO worker (aurora_3ds.cpp announces it): its own core when performance.ini fifo_core says so
    stackSize = std::max(stackSize, g_nextThreadStack); g_nextThreadStack = 0;
    Performance3ds config;
    if (FILE* f = std::fopen(N3DS_ROOT "/performance.ini", "r")) { config.read(f); std::fclose(f); }
    if (config.fifoCore != 1) {
      if (Thread t = threadCreate(reinterpret_cast<ThreadFunc>(func), arg, stackSize, prio, config.fifoCore, false)) {
        n3dsLog("[threads] FIFO worker prio 0x%lx stack %lu KB on core %d\n", (long)prio, (unsigned long)(stackSize >> 10), config.fifoCore);
        *thread = reinterpret_cast<struct __pthread_t*>(t);
        return 0;
      }
      n3dsLog("[threads] FIFO worker: core %d refused, falling back\n", config.fifoCore);
    }
  }
  // (Claude) The console refuses core 1 to threads above the default priority (svcCreateThread e0a01839, log-cia-run4) and every refused
  // attempt still takes one of the process's 32 thread slots, which then ran out: after the first refusal core 1 is not tried again.
  static bool core1Refused = [] { const char* v = std::getenv("N3DS_TEST_BG_CORE"); return v && *v == '1'; }();   // (Claude v110) test: act as if core 1 refused
  static const int bgCore = [] { Performance3ds c; if (FILE* f = std::fopen(N3DS_ROOT "/performance.ini", "r")) { c.read(f); std::fclose(f); } return c.bgCore; }();
  if (core == 1 && core1Refused) core = bgCore >= 2 ? bgCore : 0;   // (Claude v110) not core 0: the game thread's core
  // (Claude v104) console v102: core 1 accepted pthread 1 (0x30) and refused pthread 2 (0x24), so every later thread (audio/DSP emulation, loading,
  // alarms) ran on core 0 beside the game thread, above its priority. A core-1 thread is now created at 0x30 (accepted) and raised to its priority
  // afterwards; if the raise is refused it keeps 0x30 there.
  const s32 wantPrio = prio;
  const bool raiseAfter = core == 1 && prio < 0x30;
  Thread t = threadCreate(reinterpret_cast<ThreadFunc>(func), arg, stackSize ? stackSize : 0x8000, raiseAfter ? 0x30 : prio, core, false);
  if (t && raiseAfter) {
    const Result rr = svcSetThreadPriority(threadGetHandle(t), wantPrio);
    static int raised = 0;
    if (raised < 40) { raised++; n3dsLog("[threads] core 1: created at 0x30, raised to 0x%lx: %08lx\n", (long)wantPrio, (unsigned long)rr); }
  }
  int used = core;
  if (!t && core == 1) core1Refused = true;
  // (Claude v110) console v107: core 1 still refused pthreads 2..15 (even at 0x30), so 14 game threads (DSP/audio emulation, loading, alarms; several
  // above the game thread's priority) shared core 0 with the game thread. They now go to a New 3DS core (performance.ini bg_core, default 2; 0 = old).
  if (!t && core == 1 && bgCore >= 2) { t = threadCreate(reinterpret_cast<ThreadFunc>(func), arg, stackSize ? stackSize : 0x8000, prio, bgCore, false); if (t) used = bgCore; }
  if (!t && core != 0) { t = threadCreate(reinterpret_cast<ThreadFunc>(func), arg, stackSize ? stackSize : 0x8000, prio, 0, false); used = 0; }
  { static int logged = 0; if (logged < 40) { logged++; n3dsLog("[threads] pthread %d prio 0x%lx stack %lu KB on core %d%s\n", logged, (long)prio, (unsigned long)(stackSize >> 10), used, t ? "" : " (failed)"); } }
  if (!t || (used != core && used < 2)) threadFailureReport(prio, core);
  if (!t) return ENOMEM;
  *thread = reinterpret_cast<struct __pthread_t*>(t);
  return 0;
}

int n3dsPushCreatePriority(int gcPriority) {
  s32 old = 0x30;
  svcGetThreadPriority(&old, CUR_THREAD_HANDLE);
  n3dsSetThreadPriority(0, gcPriority);
  return old;
}
void n3dsPopCreatePriority(int saved) { svcSetThreadPriority(CUR_THREAD_HANDLE, saved); }

void n3dsYield(void) { g_n3dsMark[6]=g_n3dsMark[6]+1; svcSleepThread(50000); }   // 50 us: lets a thread of the same priority run (no time slicing on the 3DS)

int n3dsAptLoop(void) {
  // Opt-in cleanup-path test; never enabled by normal player settings.
  static const unsigned quitFrame=[](){const char* v=std::getenv("N3DS_TEST_QUIT_FRAME");return v?unsigned(std::strtoul(v,nullptr,10)):0u;}();
  if(quitFrame && g_frames.load()>=quitFrame){n3dsLog("[exit] requested diagnostic quit at frame %u\n",g_frames.load());return 0;}
  return aptMainLoop() ? 1 : 0;
}

namespace {
// testing: N3DS_ROOT/autopress.txt, lines "frame count KEY[+KEY...] [stickX stickY]" hold those buttons (names as in N3DS_KEY_*: A B X Y L R ZL ZR START SELECT
// DUP DDOWN DLEFT DRIGHT, or NONE) for `count` frames from `frame`. Absent for players: nothing is read then.
struct AutoPress { uint64_t first, count; u32 keys; int sx, sy; };
std::vector<AutoPress> g_auto;
bool g_autoLoaded = false;
void loadAutoPress() {
  g_autoLoaded = true;
  FILE* f = std::fopen(N3DS_ROOT "/autopress.txt", "r");
  if (!f) return;
  static const struct { const char* n; u32 k; } names[] = {{"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R}, {"ZL", KEY_ZL}, {"ZR", KEY_ZR},
      {"START", KEY_START}, {"SELECT", KEY_SELECT}, {"DUP", KEY_DUP}, {"DDOWN", KEY_DDOWN}, {"DLEFT", KEY_DLEFT}, {"DRIGHT", KEY_DRIGHT}, {"NONE", 0}};
  char line[256];
  while (std::fgets(line, sizeof line, f)) {
    unsigned long long a = 0, n = 0; char keys[128] = {}; int sx = 0, sy = 0;
    const int got = std::sscanf(line, "%llu %llu %127s %d %d", &a, &n, keys, &sx, &sy);
    if (got < 3) continue;
    AutoPress P{a, n, 0, sx, sy};
    for (char* t = std::strtok(keys, "+"); t; t = std::strtok(nullptr, "+"))
      for (const auto& nm : names) if (std::strcmp(t, nm.n) == 0) P.keys |= nm.k;
    g_auto.push_back(P);
  }
  std::fclose(f);
  n3dsLog("[3ds] autopress: %zu entries\n", g_auto.size());
}
}  // namespace

void n3dsHidRead(N3dsPad* p) {
  hidScanInput();
  u32 k = hidKeysHeld();
  static u32 menuHeld=0;static bool menuTestDone=false;static const bool menuTestRequested=std::getenv("N3DS_SETTINGS_TEST")!=nullptr;
  bool menuTouch=false;
  if((hidKeysDown()&KEY_TOUCH)&&g_perf.bottomUi&&!n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire)&&n3ds_ui::Presentation{n3ds_ui::presentationWord.load(std::memory_order_acquire)}.alpha()==255){touchPosition t{};hidTouchRead(&t);menuTouch=t.px>=20&&t.px<300&&t.py>=206&&t.py<232;}
  const bool menuTest=!menuTestDone&&menuTestRequested&&g_inputFrames.load()>=620;
  if(menuTest||menuTouch||((k&(KEY_START|KEY_SELECT))==(KEY_START|KEY_SELECT)&&(hidKeysDown()&(KEY_START|KEY_SELECT)))){
    menuTestDone=true;const bool hadAccel=n3ds_motion::enabled[1],hadGyro=n3ds_motion::enabled[2];n3dsMotionStop();try{citro_settings::runMenu(menuTest);}catch(...){n3dsLog("[settings] unable to open or save (allocation failure)\n");}if(hadAccel)n3dsMotionEnable(1,1);if(hadGyro)n3dsMotionEnable(2,1);hidScanInput();menuHeld=hidKeysHeld();std::memset(p,0,sizeof *p);return;
  }
  menuHeld&=k;k&=~menuHeld;
  if ((hidKeysDown() & KEY_SELECT) && !(k & KEY_START)) g_captureRequest.store(true);
  if (!g_autoLoaded) loadAutoPress();
  int autoX = 0, autoY = 0;
  for (const AutoPress& a : g_auto)
    if (g_inputFrames.load() >= a.first && g_inputFrames.load() < a.first + a.count) { k |= a.keys; if (a.sx || a.sy) { autoX = a.sx; autoY = a.sy; } }
  circlePosition c, cs;
  hidCircleRead(&c);
  hidCstickRead(&cs);
  std::memset(p, 0, sizeof *p);
  p->keys = k;
  p->stickX = c.dx; p->stickY = c.dy;
  if (autoX || autoY) { p->stickX = int16_t(autoX); p->stickY = int16_t(autoY); }
  p->cstickX = cs.dx; p->cstickY = cs.dy;
}

uint64_t n3dsTicks(void) { return svcGetSystemTick(); }
double n3dsTicksPerSecond(void) { return double(SYSCLOCK_ARM11); }

}  // extern "C"

// Read only completed GPU data, on the renderer owner. This recovers the clear
// depth sentinel used by sun/lens visibility without inventing numeric W->GX Z.
extern "C" int n3dsReadClearDepth(uint16_t gx, uint16_t gy) {
  struct Query { uint16_t x,y; int clear; } q{gx,gy,0};
  if (gx >= 640 || gy >= 480 || !g_tgt) return 0;
  auto read = [](void* arg) {
    auto& q=*static_cast<Query*>(arg);
    if (!g_tgt || !g_tgt->frameBuf.depthBuf || g_frames.load()==0) return;
    C3Di_RenderQueueWaitDone();
    // The screen target is 240x400; landscape x becomes buffer row, y reverses
    // buffer column. Three-bit Morton order within each 8x8 tile.
    const float squeeze = 0.8f; // renderer widescreen policy, independent of game culling switch
    const int lx=int(200.0f+(float(q.x)-320.0f)*(400.0f/640.0f)*squeeze);
    const int ly=int(float(q.y)*(240.0f/480.0f));
    if (lx<0 || lx>=400 || ly<0 || ly>=240) return;
    const unsigned bx=239-ly,by=lx;
    unsigned morton=0;for(unsigned b=0;b<3;b++)morton|=((bx>>b)&1)<<(2*b)|((by>>b)&1)<<(2*b+1);
    const size_t offset=((by>>3)*30+(bx>>3))*64+morton;
    const auto* depth=static_cast<const uint32_t*>(g_tgt->frameBuf.depthBuf);
    const uint32_t raw=depth[offset];
    q.clear=g_depthVisibility.isFar(raw);
    {static unsigned logged=0;if(logged++<24)   // (Claude v82) always: the first sun-visibility reads show the console's raw depth
     n3dsLog("[depth-peek] game %u,%u screen %d,%d raw %08lx clear %d\n",q.x,q.y,lx,ly,(unsigned long)raw,q.clear);}
  };
  if(n3dsRenderThreadActive()) n3dsRenderThreadJob(read,&q);else read(&q);
  return q.clear;
}


// (Claude v106) CPU readback of a GX copy texture the renderer made (picto box photo, photo-judging EFB peek). The copy lives only in VRAM
// (EfbCopyPlan: a render target named by the copy's destination address); this reads its picture (portW x portH, top row first) as RGBA8.
// Runs on the render thread after the GPU finished. Returns 0 without a copy at addr (or an unreadable format).
extern "C" uint32_t n3dsRenderedFrames(void) { return g_frames.load(); }
extern "C" int n3dsReadCopyRGBA(uint32_t addr, std::vector<uint8_t>* out, int* outW, int* outH) {
  struct Job { uint32_t addr; std::vector<uint8_t>* out; int w, h, ok; } j{addr, out, 0, 0, 0};
  auto run = [](void* arg) {
    Job& j = *static_cast<Job*>(arg);
    if (!g_r) return;
    C3Di_RenderQueueWaitDone();
    const t2p::TexEntry* e = g_r->cache().renderedAt(j.addr);
    if (!e || !e->user) return;
    const void* data = nullptr; uint32_t bytes = 0;
    if (!g_r->backend().textureImage(*e, &data, &bytes)) return;
    const int W = e->tex.width, H = e->tex.height, pw = e->portW, ph = e->portH, fmt = e->tex.fmt;
    const int bpp = fmt == t2p::PTF_RGBA8 ? 4 : fmt == t2p::PTF_RGB565 || fmt == t2p::PTF_RGBA4 ? 2 : 0;
    if (!bpp || pw <= 0 || ph <= 0 || pw > W || ph > H || uint32_t(W * H * bpp) > bytes) return;
    // measured (v106 orientation test): rows come out top-first in memory order; N3DS_RB_FLIP=1 restores the old bottom-first read
    static int flip = -1; if (flip < 0) { const char* v = std::getenv("N3DS_RB_FLIP"); flip = !(v && *v == '1'); }
    const uint8_t* src = static_cast<const uint8_t*>(data);
    j.out->assign(size_t(pw) * ph * 4, 0);
    for (int y = 0; y < ph; y++) {
      // render-to-texture rows are read top-first here (flip=1); the old assumption (picture top stored last) came out upside-down
      const int v = flip ? y : H - 1 - y;
      for (int x = 0; x < pw; x++) {
        unsigned morton = 0;
        for (unsigned b = 0; b < 3; b++) morton |= ((unsigned(x) >> b) & 1) << (2 * b) | ((unsigned(v) >> b) & 1) << (2 * b + 1);
        const size_t idx = (size_t(v >> 3) * (W >> 3) + (x >> 3)) * 64 + morton;
        uint8_t* o = j.out->data() + (size_t(y) * pw + x) * 4;
        if (bpp == 4) { const uint8_t* p = src + idx * 4; o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; }
        else {
          uint16_t t; std::memcpy(&t, src + idx * 2, 2);
          if (fmt == t2p::PTF_RGB565) { o[0] = uint8_t((t >> 11) * 255 / 31); o[1] = uint8_t(((t >> 5) & 63) * 255 / 63); o[2] = uint8_t((t & 31) * 255 / 31); o[3] = 255; }
          else { o[0] = uint8_t((t >> 12) * 17); o[1] = uint8_t(((t >> 8) & 15) * 17); o[2] = uint8_t(((t >> 4) & 15) * 17); o[3] = uint8_t((t & 15) * 17); }
        }
      }
    }
    j.w = pw; j.h = ph; j.ok = 1;
  };
  if (n3dsRenderThreadActive()) n3dsRenderThreadJob(run, &j); else run(&j);
  if (j.ok) { *outW = j.w; *outH = j.h; n3dsLog("[readback] copy %08lx: %dx%d\n", (unsigned long)addr, j.w, j.h); }
  else n3dsLog("[readback] copy %08lx: none\n", (unsigned long)addr);
  return j.ok;
}

// (Claude, work split v2) minimap routing receiver (ui_route_3ds.h). Stub until the route marker / bottom target land: the map stays on the top screen.
#include "ui_route_3ds.h"
// (Claude v80) the real route. Begin / End run on the game producer (where the map objects execute their GX draws); they put an ordered marker
// into the GX stream ("\x01WWROUTE1 B src[4] dst[4]" / "\x01WWROUTE1 E", parsed by GxFifo::uiRouteMarker), so the recorder and renderer see the
// route in draw order with no shared flag. The source rectangle (the map's 2D anchor and size, ChatGPT's n3dsUiMapGetSourceRect) is read here,
// on the producer, and travels in the marker. The renderer (core 2) owns the bottom target and reports ownership through n3dsUiSetGpuOwnsBottom.
extern "C" void GXInsertDebugMarker(const char* label);
extern "C" int n3dsUiMapGetSourceRect(int* left, int* top, int* width, int* height) __attribute__((weak));
static bool g_mapRouteOpen = false;   // producer thread only
// (Claude) d_meter.cpp asks this so the minimap's frame/wind arrow/buttons are hidden on the top screen while the map itself is routed to
// the bottom screen: the same conditions n3dsUiMapBegin routes under
extern "C" int n3dsUiMapRoutedToBottom(void) {
  return g_perf.uiMapBottom && g_perf.bottomUi && g_r && n3dsUiMapGetSourceRect &&
         n3ds_ui::Presentation{n3ds_ui::presentationWord.load(std::memory_order_acquire)}.alpha();
}
extern "C" bool n3dsUiMapBegin(int left, int top, int width, int height) {
  if (!g_perf.uiMapBottom || !g_perf.bottomUi || !g_r || g_mapRouteOpen || !n3dsUiMapGetSourceRect) return false;
  if(!n3ds_ui::Presentation{n3ds_ui::presentationWord.load(std::memory_order_acquire)}.alpha())return false;
  if (width <= 0 || height <= 0 || left < 0 || top < 0 || left + width > 320 || top + height > 240) return false;
  int sl = 0, st = 0, sw = 0, sh = 0;
  if (!n3dsUiMapGetSourceRect(&sl, &st, &sw, &sh) || sw <= 0 || sh <= 0 || sw > 4096 || sh > 4096 || sl < -4096 || sl > 4096 || st < -4096 || st > 4096) return false;
  char m[96];
  std::snprintf(m, sizeof m, "\x01WWROUTE1 B %d %d %d %d %d %d %d %d", sl, st, sw, sh, left, top, width, height);
  GXInsertDebugMarker(m);
  g_mapRouteOpen = true;
  return true;
}
extern "C" void n3dsUiMapEnd(void) {
  if (!g_mapRouteOpen) return;
  GXInsertDebugMarker("\x01WWROUTE1 E");
  g_mapRouteOpen = false;
}

extern "C" int n3dsBatchFx(void) { return g_perf.batchFx ? 1 : 0; }
// (Claude v103) frame skip: called by fpcM_Management at the start of each logic frame. Returns 1 = do not paint this frame (no GX encoding,
// decode, render or present; game logic and the actor draw pass still run). Only while the game is more than half a frame behind its
// 30 Hz schedule, at most g_perf.frameSkip frames in a row. Game speed (and cutscene audio sync) then stays close to real time.
static bool g_paintSkipped=false;
static uint32_t g_logicFrames=0,g_skippedFrames=0,g_drawsSkipped=0;
// (Claude v114) skip_draw: the decision for frame N+1's paint is made before frame N's actor draw pass (the painter at the start of N+1 paints
// the lists that draw pass builds). A frame that will not be painted then does not build them either (draw-all ~14 ms busy Outset on console).
static uint64_t g_skipIdeal=0; static int g_skipRun=0; static bool g_skipDecided=false,g_skipNext=false;
static uint64_t g_drawStart=0; static int64_t g_drawEma=0; static bool g_actorDrawSkipped=false;
static bool skipForced(){ static const bool force=std::getenv("N3DS_SKIP_FORCE")!=nullptr; return force; }   // test: skip every other frame regardless of time
static int legacyBeginLogicFrame(void) {
  const uint64_t now=svcGetSystemTick(); const int64_t period=int64_t(SYSCLOCK_ARM11/30);
  if(!g_skipIdeal)g_skipIdeal=now;
  int64_t lag=int64_t(now-g_skipIdeal);
  if(lag>4*period||lag<-period){g_skipIdeal=now;lag=0;}   // loading / long stall: the debt is written off; idle: no credit
  bool skip=skipForced() ? g_skipRun<1 : g_perf.frameSkip>0 && lag>period/2 && g_skipRun<g_perf.frameSkip;
  if(g_skipDecided){skip=g_skipNext;g_skipDecided=false;}   // decided before the last draw pass (skip_draw): must be kept
  g_skipRun=skip?g_skipRun+1:0;
  g_skipIdeal+=uint64_t(period);
  g_paintSkipped=skip;
  ++g_logicFrames; if(skip)++g_skippedFrames;
  if(g_logicFrames%300==0){n3dsLog("[frame-skip] logic frames %lu, not painted %lu of the last 300, draw passes left out %lu\n",(unsigned long)g_logicFrames,(unsigned long)g_skippedFrames,(unsigned long)g_drawsSkipped);g_skippedFrames=0;g_drawsSkipped=0;}
  return skip?1:0;
}
// called by fpcM_Management before the actor draw pass: 1 = leave it out (the next frame is then not painted)
static int legacySkipDrawPass(void) {
  g_drawStart=svcGetSystemTick(); g_actorDrawSkipped=false;
  if(!g_perf.skipDraw||(!skipForced()&&g_perf.frameSkip<=0))return 0;
  const int64_t period=int64_t(SYSCLOCK_ARM11/30);
  const int64_t lag=int64_t(g_drawStart-g_skipIdeal)+g_drawEma;   // the next frame's lag at its start if this draw pass runs
  const bool skip=skipForced() ? g_skipRun<1 : lag>period/2 && lag<=4*period && g_skipRun<g_perf.frameSkip;
  g_skipDecided=true; g_skipNext=skip;
  if(skip)++g_drawsSkipped;
  g_actorDrawSkipped=skip;
  return skip?1:0;
}

// (Claude v120) paint pacing (performance.ini paint_pacing=1, default). v103-v119 skipped at most frame_skip ticks in a row and otherwise
// painted: a painted tick then waited for the renderer (GXDrawDone's FIFO drain behind the worker's hand-off to the busy render core),
// so on the console logic ticks took 43-106 ms (v117 log) and game speed, camera and movement followed the paint rate (1/3-3/4 speed,
// varying from moment to moment). Now:
//  - logic runs on an absolute 30 Hz clock: a tick that is early sleeps until its slot (waitForTick's relative limiter is off,
//    pc_frame.cpp n3dsOwnsPacing); a late one runs at once, unpainted ticks catching up the debt (written off past 8 ticks: loading);
//  - a tick is painted only when the hand-off of the frame before it will not stall the FIFO worker (render core done by the time this
//    paint starts: n3dsRenderRemainingTicks), so the game waits only for its own frame's decode;
//  - paints keep a steady cadence: every K ticks (K adapts 1..4: up when paints keep coming late, down when the renderer has been free
//    a tick early for a while), so motion advances the same amount per painted frame instead of 1-2-1-2 jumps. paint_max_gap bounds it.
extern "C" void n3dsPaintEnded(void) { g_paintEndedN.fetch_add(1, std::memory_order_relaxed); }
extern "C" void n3dsPaintHandedOff(void) { g_paintHandedN.fetch_add(1, std::memory_order_release); }
extern "C" uint64_t n3dsRenderRemainingTicks(uint64_t now);   // render_thread_3ds.cpp
extern "C" uint64_t n3dsRenderFrameTicks(void);
static std::atomic<uint32_t> g_procSlow{0}, g_procDev{0};   // FIFO worker: decode time per painted frame, slow average + upward deviation
extern "C" void n3dsPacingNoteProcess(uint64_t ticks) {
  const uint32_t d = ticks > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(ticks);
  const uint32_t a = g_procSlow.load(std::memory_order_relaxed);
  if (!a) { g_procSlow.store(d, std::memory_order_relaxed); return; }
  const uint32_t na = uint32_t((uint64_t(a) * 15 + d) / 16);
  g_procSlow.store(na, std::memory_order_relaxed);
  const uint32_t up = d > na ? d - na : 0;
  g_procDev.store(uint32_t((uint64_t(g_procDev.load(std::memory_order_relaxed)) * 15 + up) / 16), std::memory_order_relaxed);
}
// (Claude v120) present pacing: called by the renderer right before C3D_FrameEnd. Painted ticks are K x 33.3 ms apart; the render core's
// finish time after its tick varies (decode, draws), so the frame is held until slot + (slow average + 2 x upward deviation) of that
// delay, at most 30 ms: display intervals follow the even tick cadence instead of the render time.
extern "C" { uint64_t g_presentHeldTicks = 0; }   // render core: this frame's hold (render_thread_3ds.cpp leaves it out of the render time)
extern "C" void n3dsPresentPace(void) {
  if (!g_perf.paintPacing || !g_perf.presentPace) return;
  const uint64_t slot = g_renderSlot;
  if (!slot) return;
  static double ema = 0, dev = 0; static uint64_t slept = 0; static unsigned n = 0, held = 0;
  const uint64_t now = svcGetSystemTick();
  const double period = double(SYSCLOCK_ARM11) / 30.0;
  double lat = double(int64_t(now - slot));
  if (lat < 0) lat = 0;
  if (lat > 6 * period) { ema = dev = 0; return; }   // stall / loading: start over
  if (ema > 0) {
    const double target = ema + 2 * dev;
    if (lat < target) {
      double w = target - lat; const double cap = double(SYSCLOCK_ARM11) * 0.030;
      if (w > cap) w = cap;
      svcSleepThread(int64_t(w * 1e9 / SYSCLOCK_ARM11)); slept += uint64_t(w); ++held; g_presentHeldTicks = uint64_t(w);
    }
    ema = ema * 15.0 / 16.0 + lat / 16.0;
    const double up = lat > ema ? lat - ema : 0; dev = dev * 15.0 / 16.0 + up / 16.0;
  } else ema = lat;
  if (++n == 300) {
    n3dsLog("[present-pace] 300 frames: render done %.1f ms after its tick (+%.1f dev), held %u frames, %.1f ms each\n", ema * 1000.0 / SYSCLOCK_ARM11, dev * 1000.0 / SYSCLOCK_ARM11, held, held ? double(slept) * 1000.0 / SYSCLOCK_ARM11 / held : 0.0);
    n = held = 0; slept = 0;
  }
}
extern "C" int n3dsOwnsPacing(void) { return g_perf.paintPacing ? 1 : 0; }   // pc_frame.cpp: pc_frame_pace does not sleep
namespace pacing {
n3ds::PaintBudget paintBudget;
int since = 0;            // logic ticks since the last painted tick began
int k = 1;                // cadence
int late = 0, early = 0, paints = 0, firstReady = 0;
uint64_t prevBegin = 0;   // when the last tick began
// stats per 300 ticks
uint64_t winStart = 0; uint32_t winTicks = 0, winPaints = 0, winForced = 0, winNotReady = 0, winKSum = 0;
uint64_t sleptTicks = 0, writtenOff = 0, lagSum = 0, lagMax = 0;
uint32_t gapHist[9] = {};
bool ready(uint64_t now, int64_t horizon) {
  const uint32_t inflight = g_paintEndedN.load(std::memory_order_relaxed) - g_paintHandedN.load(std::memory_order_acquire);
  if (inflight == 0) return true;
  const int64_t margin = int64_t(SYSCLOCK_ARM11 / 1000);   // 1 ms: the hand-off's own work
  return int64_t(n3dsRenderRemainingTicks(now)) + margin <= horizon;
}
// K from what a painted frame costs the two cores that cannot overlap two frames: the render core (render + present) and the FIFO
// worker (decode). Up at once when a frame needs more than K ticks (plus 5% slack), down only after 30 paints that would fit in K-1
// ticks with 15% to spare; paints that still come late (renderer busy at the cadence) push it up as well.
void adapt(int gap) {
  const double period = double(SYSCLOCK_ARM11) / 30.0;
  const double need = double(std::max<uint64_t>(n3dsRenderFrameTicks(), uint64_t(g_procSlow.load(std::memory_order_relaxed)) + g_procDev.load(std::memory_order_relaxed)));
  int want = 1;
  const int kMax = std::min(4, g_perf.paintMaxGap);
  while (want < kMax && need > double(want) * period * 0.95) ++want;
  if (want > k) { k = want; early = 0; late = 0; paints = 0; return; }
  if (gap > k) ++late;
  if (++paints >= 8) { if (late >= 4 && k < kMax) { ++k; early = 0; } paints = 0; late = 0; }
  if (k > 1 && need < double(k - 1) * period * 0.85) { if (++early >= 30) { --k; early = 0; paints = 0; late = 0; } }
  else early = 0;
}
// the paint decision for the next tick to begin; horizon = ticks until its painter starts
bool decide(uint64_t now, int64_t horizon, int64_t lag) {
  const int64_t period = int64_t(SYSCLOCK_ARM11 / 30);
  const int gap = since + 1;
  const bool r = ready(now, horizon);
  if (r && !firstReady) firstReady = gap;
  const int K = g_perf.paintCadence ? g_perf.paintCadence : k;
  const bool catchUp = lag > 2 * period;   // well behind: logic first, one more tick between paints
  const int need = catchUp ? K + 1 : K;
  const bool forced = gap >= g_perf.paintMaxGap;
  bool paint;
  if(g_perf.paintFractional && !g_perf.paintCadence){
    const uint64_t work=std::max<uint64_t>(n3dsRenderFrameTicks(),uint64_t(g_procSlow.load(std::memory_order_relaxed))+g_procDev.load(std::memory_order_relaxed));
    // Leave5% scheduling headroom, retaining the existing max-gap safeguard.
    const int64_t cost=std::min<int64_t>(period*g_perf.paintMaxGap,std::max<int64_t>(period,int64_t(work+work/20)));
    paint=paintBudget.decide(period,cost,r,forced,catchUp);
  } else paint=(r && gap>=need)||forced;
  if (!paint) { if (gap >= need) ++winNotReady; return false; }
  if (forced && !r) ++winForced;
  ++gapHist[gap < 8 ? gap : 8];
  if (!g_perf.paintCadence) adapt(gap);
  firstReady = 0;
  return true;
}
}  // namespace pacing
extern "C" long long OSGetTime(void); extern "C" unsigned int VIGetRetraceCount(void);
extern "C" int n3dsBeginLogicFrame(void) {
  if (!g_perf.paintPacing) return legacyBeginLogicFrame();
  {static bool logged=false;if(!logged){logged=true;n3dsLog("[pacing] paint_fractional %d\n",int(g_perf.paintFractional));}}
  using namespace pacing;
  const int64_t period = int64_t(SYSCLOCK_ARM11 / 30);
  uint64_t now = svcGetSystemTick();
  if (!g_skipIdeal) g_skipIdeal = now;
  int64_t lag = int64_t(now - g_skipIdeal);
  if (lag < 0) {   // early: sleep to this tick's slot
    const int64_t wait = -lag < 2 * period ? -lag : 2 * period;
    svcSleepThread(int64_t(wait * 1000000000.0 / SYSCLOCK_ARM11));
    sleptTicks += uint64_t(wait);
    now = svcGetSystemTick(); lag = int64_t(now - g_skipIdeal);
    if (lag < -period) { g_skipIdeal = now; lag = 0; }
  }
  if (lag > 2 * period && lag <= 8 * period) {   // (v121) no catch-up bursts: at most ~1 tick of debt is kept (the rest is written off)
    writtenOff += uint64_t(lag - period); g_skipIdeal = now - uint64_t(period); lag = period;
  }
  if (lag > 8 * period) {   // loading / long stall: debt written off
    n3dsLog("[pacing] stall: logic frame %lu began %.0f ms late (previous tick took %.0f ms, painted %d)\n", (unsigned long)g_logicFrames, double(lag) * 1000.0 / SYSCLOCK_ARM11,
            pacing::prevBegin ? double(now - pacing::prevBegin) * 1000.0 / SYSCLOCK_ARM11 : 0.0, int(!g_paintSkipped));
    writtenOff += uint64_t(lag); g_skipIdeal = now; lag = 0;
  }
  if (lag < 0) lag = 0;
  prevBegin = now;
  bool skip;
  if (skipForced()) skip = since < 1 ? true : false;
  else if (g_skipDecided) skip = g_skipNext;
  else skip = !decide(now, 0, lag);
  g_skipDecided = false;
  if (!skip) g_paintSlotRing[g_paintEndedN.load(std::memory_order_relaxed) & 7] = g_skipIdeal;   // this tick's slot (lag >= 0 here)
  since = skip ? since + 1 : 0;
  g_skipRun = skip ? g_skipRun + 1 : 0;
  g_skipIdeal += uint64_t(period);
  g_paintSkipped = skip;
  ++g_logicFrames; if (skip) ++g_skippedFrames;
  lagSum += uint64_t(lag); if (uint64_t(lag) > lagMax) lagMax = uint64_t(lag);
  if (!winStart) winStart = now;
  ++winTicks; if (!skip) ++winPaints; winKSum += uint32_t(g_perf.paintCadence ? g_perf.paintCadence : k);
  if (winTicks == 300) {
    const double ms = 1000.0 / SYSCLOCK_ARM11; const double el = double(now - winStart) * ms;
    n3dsLog("[pacing] 300 ticks in %.0f ms: game speed %.0f%%, painted %u (%.1f fps), cadence K avg %.2f, paint gaps 1:%u 2:%u 3:%u 4:%u 5:%u 6+:%u, "
            "waited for renderer %u, renderer busy at cadence %u, slept %.0f ms, lag mean %.1f max %.1f ms, written off %.0f ms\n",
            el, el > 0 ? 300.0 * 1000.0 / 30.0 / el * 100.0 : 0.0, winPaints, el > 0 ? winPaints * 1000.0 / el : 0.0, winKSum / 300.0,
            gapHist[1], gapHist[2], gapHist[3], gapHist[4], gapHist[5], gapHist[6] + gapHist[7] + gapHist[8],
            winForced, winNotReady, double(sleptTicks) * ms, double(lagSum) * ms / 300, double(lagMax) * ms, double(writtenOff) * ms);
    if(g_perf.paintFractional)n3dsLog("[pacing-budget] fractional on; credit %.2f ms (K above is the unused legacy estimate)\n",double(paintBudget.credit)*ms);
    {   // (Claude v123 diag) does the game's OS clock run at real speed? OSGetTime ticks are OS_TIMER_CLOCK (40.5 MHz)
      static int64_t osPrev = 0; static uint64_t sysPrev = 0; const int64_t os = OSGetTime();
      if (osPrev) n3dsLog("[clock] OSGetTime advanced %.0f ms, 3DS system clock %.0f ms, retraces %u\n", double(os - osPrev) / 40500.0,
                          double(now - sysPrev) * 1000.0 / SYSCLOCK_ARM11, (unsigned)VIGetRetraceCount());
      osPrev = os; sysPrev = now;
    }
    winStart = now; winTicks = winPaints = winForced = winNotReady = winKSum = 0; sleptTicks = writtenOff = lagSum = lagMax = 0;
    for (auto& g : gapHist) g = 0;
  }
  return skip ? 1 : 0;
}
// called by fpcM_Management before the actor draw pass: 1 = leave it out (the next tick is then not painted)
extern "C" int n3dsSkipDrawPass(void) {
  if (!g_perf.paintPacing) return legacySkipDrawPass();
  g_drawStart = svcGetSystemTick(); g_actorDrawSkipped = false;
  if (!g_perf.skipDraw) return 0;
  bool skip;
  if (skipForced()) skip = pacing::since == 0;   // test: every other tick
  else {
    // the next tick's painter starts after this draw pass, or at its 30 Hz slot if that is later (g_skipIdeal is already that slot)
    const int64_t untilSlot = int64_t(g_skipIdeal - g_drawStart);
    const int64_t horizon = g_drawEma > untilSlot ? g_drawEma : untilSlot;
    const int64_t lagNext = -untilSlot + g_drawEma;   // the next tick's lag at its start
    skip = !pacing::decide(g_drawStart, horizon, lagNext > 0 ? lagNext : 0);
  }
  g_skipDecided = true; g_skipNext = skip;
  if (skip) ++g_drawsSkipped;
  g_actorDrawSkipped = skip;
  return skip ? 1 : 0;
}
extern "C" int n3dsEarlyReleaseOn(void) { return g_perf.earlyRelease ? 1 : 0; }
extern "C" int n3dsRealShadowsOff(void) { return g_perf.realShadows ? 0 : 1; }
extern "C" int n3dsActorDrawSkipped(void) { return g_actorDrawSkipped ? 1 : 0; }
extern "C" void n3dsDrawPassDone(void) {
  if(g_actorDrawSkipped)return;   // the estimate is of a full draw pass
  const int64_t t=int64_t(svcGetSystemTick()-g_drawStart);
  g_drawEma=g_drawEma?(g_drawEma*7+t)/8:t;
}
extern "C" int n3dsPaintSkipped(void) { return g_paintSkipped ? 1 : 0; }   // (Claude v102) d_kankyo_rain.cpp
extern "C" float n3dsFoliageFar(float original) { const int ab = g_foliageAb.load(std::memory_order_relaxed); const int lim = ab >= 0 ? ab : g_perf.foliageFar; return lim > 0 && float(lim) < original ? float(lim) : original; }
extern "C" int n3dsJ3dProgramsOn(void) { return g_perf.j3dPrograms ? 1 : 0; }

extern "C" int n3dsJ3dGeometryOn(void) { return g_perf.j3dGeometry ? 1 : 0; }

extern "C" void n3dsSettingsRebase(uint64_t elapsed){
  // Modal time is not simulation debt or a slow rendering frame.
  if(g_skipIdeal)g_skipIdeal+=elapsed;if(g_drawStart)g_drawStart+=elapsed;
  if(pacing::prevBegin)pacing::prevBegin+=elapsed;if(pacing::winStart)pacing::winStart+=elapsed;
  for(auto& slot:g_paintSlotRing)if(slot)slot+=elapsed;
}
