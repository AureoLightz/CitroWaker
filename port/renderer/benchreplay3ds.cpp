// benchreplay3ds: renderer-only ARM replay benchmark (Claude, 2026-11-10).
// Replays PC .wwrp recordings of the wide-Outset fixture through the REAL Renderer3ds on the 3DS
// (or Azahar), configured like n3ds/src/gpu_3ds.cpp, and writes per-phase averages to
//   sdmc:/3ds/ww3ds/claude-replay/results.txt
// Inputs in sdmc:/3ds/ww3ds/claude-replay/: bundle.bin, <capture>.wwrp, config.txt with lines
//   capture NAME            (cliff / aerial ...; may repeat: every phase runs on every capture)
//   passes WARM MEASURED    (default 2 4)
//   phase LABEL [key=0/1 ...]   keys: timing lean runs vplan vdecode planreuse fogprep packed prof retain
// Each phase: fresh Renderer3ds (cold caches, as a game boot), WARM unmeasured passes, MEASURED passes.
// Reports process/render(build/plan/draw) ms per frame, allocations per frame, draws, pixel hash.
#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <algorithm>
#include <cstdarg>
#include <exception>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "renderer3ds.h"
namespace t2p { extern uint64_t g_replayProf[16]; extern bool g_replayProfOn; }   // (Claude v119) renderer3ds.cpp

extern "C" void C3Di_RenderQueueWaitDone(void);
extern "C" { u32 __ctru_heap_size = 100u << 20; u32 __ctru_linear_heap_size = 18u << 20; u32 __stacksize__ = 0x200000; }   // stack as the game (main_3ds.cpp): renderer paths are deep

// --- allocation counting (link with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign)
static uint32_t g_allocs = 0;   // the wrapped allocators are external calls, so plain increments are not optimized away
// (Claude v122) the game's fast_alloc (main_3ds.cpp fa::, copied): phase key fa=1 turns it on for the rest of the run, as the game runs
namespace fa {
constexpr size_t kSpan = 64u << 10;
constexpr int kClasses = 12;
constexpr unsigned kFaMaxSpans = 160;                 // 10 MiB
static const uint16_t kSize[kClasses] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024};   // (Claude v79: a 1408 B event class exhausted the span budget in the game: reverted)
static uint8_t kClassOf[65];                          // (n + 15) / 16 -> class
struct Block { Block* next; };
struct Central { LightLock lock; Block* head = nullptr; unsigned count = 0; uint8_t* bump = nullptr; uint8_t* end = nullptr; };
static Central g_central[kClasses];
static uint8_t* g_base = nullptr; static uint8_t* g_end = nullptr;
static uint8_t g_spanClass[2048];                     // (p - g_base) >> 16 -> class + 1 (0: not a span)
static std::atomic<unsigned> g_spans{0};
static std::atomic<bool> g_on{false};
std::atomic<unsigned long> g_fallbacks{0};
struct Cache { Block* head[kClasses] = {}; uint16_t n[kClasses] = {}; };
static thread_local Cache t_cache;
static inline int spanClassOf(const void* p) {
  const uint8_t* q = static_cast<const uint8_t*>(p);
  if (q < g_base || q >= g_end) return -1;
  return int(g_spanClass[size_t(q - g_base) >> 16]) - 1;
}
static bool refill(int c) {                           // central lock held by the caller: up to 32 blocks into this thread's cache
  Central& C = g_central[c]; Cache& t = t_cache;
  unsigned moved = 0;
  while (C.head && moved < 32) { Block* b = C.head; C.head = b->next; C.count--; b->next = t.head[c]; t.head[c] = b; moved++; }
  while (moved < 32) {
    if (C.bump + kSize[c] > C.end) {
      if (g_spans.load() >= kFaMaxSpans) break;
      uint8_t* s = static_cast<uint8_t*>(memalign(kSpan, kSpan));
      if (!s) break;
      if (s < g_base || s + kSpan > g_end) { free(s); break; }   // outside the table: give it back (cannot happen with the sbrk heap)
      g_spanClass[size_t(s - g_base) >> 16] = uint8_t(c + 1);
      g_spans.fetch_add(1);
      C.bump = s; C.end = s + kSpan;
    }
    Block* b = reinterpret_cast<Block*>(C.bump); C.bump += kSize[c];
    b->next = t.head[c]; t.head[c] = b; moved++;
  }
  t.n[c] = uint16_t(t.n[c] + moved);
  return moved > 0;
}
static inline void* alloc(size_t n) {
  if (!g_on.load(std::memory_order_relaxed) || n > 1024) return nullptr;
  const int c = kClassOf[(n + 15) >> 4];
  Cache& t = t_cache;
  if (!t.head[c]) {
    Central& C = g_central[c];
    LightLock_Lock(&C.lock); const bool ok = refill(c); LightLock_Unlock(&C.lock);
    if (!ok) { g_fallbacks.fetch_add(1, std::memory_order_relaxed); return nullptr; }
  }
  Block* b = t.head[c]; t.head[c] = b->next; t.n[c]--;
  return b;
}
static inline void release(void* p, int c) {
  Cache& t = t_cache;
  Block* b = static_cast<Block*>(p); b->next = t.head[c]; t.head[c] = b;
  if (++t.n[c] > 128) {                               // give 64 back to the class
    Block* first = t.head[c]; Block* last = first;
    for (int i = 1; i < 64; i++) last = last->next;
    t.head[c] = last->next; t.n[c] = uint16_t(t.n[c] - 64);
    Central& C = g_central[c];
    LightLock_Lock(&C.lock); last->next = C.head; C.head = first; C.count += 64; LightLock_Unlock(&C.lock);
  }
}
}  // namespace fa
extern "C" char* fake_heap_start; extern "C" char* fake_heap_end;
static void benchFastAllocEnable() {
  if (fa::g_on.load()) return;
  for (int i = 0, c = 0; i <= 64; i++) { while (c < fa::kClasses - 1 && size_t(i) * 16 > fa::kSize[c]) c++; fa::kClassOf[i] = uint8_t(c); }
  for (auto& C : fa::g_central) LightLock_Init(&C.lock);
  fa::g_base = reinterpret_cast<uint8_t*>(uintptr_t(fake_heap_start) & ~uintptr_t(fa::kSpan - 1));
  fa::g_end = fa::g_base + sizeof fa::g_spanClass * fa::kSpan;
  if (reinterpret_cast<uint8_t*>(fake_heap_end) > fa::g_end) return;
  fa::g_on.store(true);
}
extern "C" {
void* __real_malloc(size_t); void* __real_calloc(size_t, size_t); void* __real_realloc(void*, size_t); void* __real_memalign(size_t, size_t); void __real_free(void*);
void* __wrap_malloc(size_t n) { g_allocs++; if (void* p = fa::alloc(n)) return p; return __real_malloc(n); }
void* __wrap_calloc(size_t a, size_t b) { g_allocs++; if (b && a <= 1024 / b) if (void* p = fa::alloc(a * b)) { std::memset(p, 0, a * b); return p; } return __real_calloc(a, b); }
void* __wrap_realloc(void* p, size_t n) { g_allocs++;
  if (p) if (const int c = fa::spanClassOf(p); c >= 0) {
    if (n && n <= fa::kSize[c]) return p;
    void* q = n ? __wrap_malloc(n) : nullptr;
    if (n && !q) return nullptr;
    if (q) std::memcpy(q, p, std::min<size_t>(n, fa::kSize[c]));
    fa::release(p, c);
    return q;
  }
  return __real_realloc(p, n); }
void* __wrap_memalign(size_t a, size_t n) { g_allocs++; return __real_memalign(a, n); }
void __wrap_free(void* p) { if (p) if (const int c = fa::spanClassOf(p); c >= 0) { fa::release(p, c); return; } __real_free(p); }
}

namespace {
const char* kDir = "sdmc:/3ds/ww3ds/claude-replay/";

struct Memory : t2p::GuestMem {
  std::vector<uint8_t> bytes;
  const uint8_t* read(uint32_t addr, size_t n) const override {
    addr &= 0x03ffffff;
    return uint64_t(addr) + n <= bytes.size() ? bytes.data() + addr : nullptr;
  }
};

std::vector<uint8_t> load(const std::string& path) {
  std::vector<uint8_t> b;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return b;
  fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  if (n > 0) { b.resize(n); if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear(); }
  fclose(f);
  return b;
}
uint32_t u32at(const std::vector<uint8_t>& b, size_t& at) { uint32_t v = 0; if (at + 4 <= b.size()) memcpy(&v, b.data() + at, 4); at += 4; return v; }
double ms(u64 t) { return double(t) * 1000.0 / SYSCLOCK_ARM11; }

struct Phase { int rigidclip=0; int rkeys = 1; std::string label; int timing = 0, lean = 0, runs = 1, vplan = 1, vdecode = 1, planreuse = 1, fogprep = 1, packed = 1, prof = 0, retain = 0, staterep = 0, inplace = 0, finrep = 0, planrep = 0, skip = 0, unifrep = 0, framerep = 0, tevrep = 0, ccrep = 0, fullplanrep = 0, smemo = 1, texrep = 0, vdecrep = 0, drawreplay = 0, resident = 0, flushall = 1, rprof = 0, record = 1, kick = 0, split = 0, mbatch = 0, ft = 0, spool = 0, fa = 0, mbprof = 0; };

FILE* g_log = nullptr;
void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void out(const char* fmt, ...) {
  va_list a; va_start(a, fmt);
  if (g_log) { va_list b; va_copy(b, a); vfprintf(g_log, fmt, b); va_end(b); fflush(g_log); }
  vprintf(fmt, a); va_end(a);
}

int runPhase(const Phase& ph, const std::string& capName, const std::vector<uint8_t>& stream, const std::vector<uint8_t>& bundle,
             int warm, int measured, C3D_RenderTarget* target) {
  size_t h = 4;
  const uint32_t version = u32at(stream, h), memsize = u32at(stream, h), chunks = u32at(stream, h);
  // guest memory only up to the highest byte the recording writes (recordings declare 48 MiB)
  uint32_t used = 0, framesPerPass = 0;
  for (size_t at = 16, c = 0; c < chunks && at + 4 <= stream.size(); c++) {
    const uint8_t type = stream[at]; at += 4;
    if (type == 2) framesPerPass++;
    if (type == 0) { const uint32_t n = u32at(stream, at); at += n; }
    else if (type == 1) { const uint32_t a = u32at(stream, at), n = u32at(stream, at); used = std::max(used, std::min(memsize, a + n)); at += n; }
    else if (type == 2) at += 4;
    else return 8;
  }
  auto mem = std::make_unique<Memory>();
  mem->bytes.assign((used + 0xFFFFu) & ~0xFFFFu, 0);
  out("capture %s: guest memory %u KB of %u declared, heap free %u KB\n", capName.c_str(), unsigned(mem->bytes.size() >> 10), unsigned(memsize >> 10), unsigned(mallinfo().fordblks >> 10));
  t2p::RendererOptions o;
  o.frame.efbW = 640; o.frame.efbH = 480; o.frame.targetW = 400; o.frame.targetH = 240;
  o.frame.fit = t2p::FitMode::Stretch; o.frame.widescreen = true; o.frame.rotate = true;
  o.materialCacheFastPath = true; o.translate.packedUniforms = ph.packed; o.texCache.memoPalettes = true;
  o.packetNotes = false; o.texCache.budgetBytes = 9u << 20;
  auto r = std::make_unique<t2p::Renderer3ds>();
  out("  step: renderer allocated\n");
  if (!r->init(o, mem.get(), target)) { out("init failed\n"); return 4; }
  out("  step: renderer init\n");
  std::vector<std::string> seen;
  r->setLogs([&](const t2p::DrawReport& D) {   // same work as gpu_3ds.cpp's draw log (no file output)
    const double t = D.buildMs + D.planMs + D.drawMs;
    if (!D.drawn && seen.size() < 100 && std::find(seen.begin(), seen.end(), D.why) == seen.end()) seen.push_back(D.why);
    (void)t;
  }, nullptr, [](const std::string&) {});
  Memory* m = mem.get();
  r->fifo().setGuestArrayReader([m](uint32_t a, uint32_t n) { return m->read(a, n); });
  r->fifo().setAuroraMode(version == 2);
  r->setPlanReuse(ph.planreuse); r->setFogPreparation(ph.fogprep);
  r->fifo().setPrimitiveRuns(ph.runs); r->fifo().setGeometryCache(false);
  r->fifo().setFastAttributeDecode(ph.vdecode); r->fifo().setCachedVertexPlan(ph.vplan);
  r->setDrawTimingEnabled(ph.timing);
  r->setStateNotes(!ph.lean);
  r->setRetainGeometry(ph.retain);
  r->setStateDecodeRepeat(ph.staterep);
  r->setStateInPlace(ph.inplace);
  if (ph.resident) r->setResidentGeometry(true, size_t(ph.resident) << 10);   // (Claude v117) KB; needs retain=1
  r->setFlushAllAtEnd(ph.flushall != 0);
  r->setDrawRecord(ph.record != 0);
  r->setRigidClip(ph.rigidclip!=0);
  r->setMatrixBatch(ph.mbatch != 0);
  r->setStatePool(ph.spool != 0);   // (Claude v122)
  if (ph.fa) benchFastAllocEnable();
  r->setMatrixBatchProfile(ph.mbprof != 0);   // (Claude v122) as the game's matrix_batch
  r->backend().setKickDraws(ph.kick);   // (Claude v120) mid-frame GPU start
  if (ph.split) r->backend().setSplitStep(ph.split / 100.0f);
  if (ph.drawreplay) r->setDrawReplay(true, size_t(ph.drawreplay > 1 ? ph.drawreplay : 1536) << 10);
  g_benchRepeat[0] = ph.finrep; g_benchRepeat[1] = ph.planrep; g_benchRepeat[2] = ph.unifrep; g_benchRepeat[3] = ph.framerep; g_benchRepeat[4] = ph.tevrep; g_benchRepeat[5] = ph.ccrep; g_benchRepeat[6] = ph.fullplanrep; t2p::setStateDecodeMemo(ph.smemo != 0); g_benchRepeat[7] = ph.texrep; g_benchRepeat[8] = ph.vdecrep;
  std::string err;
  r->loadMaterialBundle(bundle.data(), bundle.size(), &err);
  out("  step: bundle loaded (%s)\n", err.c_str());

  double process = 0, render = 0, build = 0, plan = 0, draw = 0, gpuWait = 0;
  uint64_t clipTests=0,clipCulled=0;
  uint64_t aProc = 0, aRender = 0, draws = 0, gx = 0;
  uint64_t rpHits = 0, rpMiss = 0, rpRec = 0, rpGroup = 0, rpStale = 0, rpIns = 0, rsHit = 0, rsMiss = 0, rsFull = 0;
  uint64_t ftSplit[8] = {}, ftMat[4] = {};
  uint64_t why0[4]; std::memcpy(why0, t2p::g_replayWhy, sizeof why0);   // (Claude v117)
  unsigned frames = 0, skippedFrames = 0;
  for (int pass = 0; pass < warm + measured && aptMainLoop(); pass++) {
    // guest memory keeps its contents between passes; a chunk only invalidates textures it changes (as in the game)
    size_t at = 16;
    double pending = 0; uint64_t pendingA = 0;
    uint32_t frameInPass = 0;
    const bool meas = pass >= warm;
    g_t2pProfOn = meas && ph.prof;
    t2p::g_replayProfOn = meas && ph.rprof;
    if (pass == warm) for (uint64_t& v : t2p::g_replayProf) v = 0;
    if (pass == warm) std::memcpy(why0, t2p::g_replayWhy, sizeof why0);
    if (pass == warm) t2p::matrixbatch::prof() = t2p::matrixbatch::Prof();
    if (pass == warm) { for (uint64_t& v : g_t2pProf) v = 0; r->takeRetainStats(); }
    for (uint32_t c = 0; c < chunks; c++) {
      if (at + 4 > stream.size()) return 5;
      const uint8_t type = stream[at]; at += 4;
      if (type == 0) {
        const uint32_t n = u32at(stream, at);
        if (uint64_t(at) + n > stream.size()) return 6;
        const uint32_t a0 = g_allocs; const u64 t = svcGetSystemTick();
        r->fifo().process(stream.data() + at, n);
        pending += ms(svcGetSystemTick() - t); pendingA += g_allocs - a0;
        at += n;
      } else if (type == 1) {
        const uint32_t a = u32at(stream, at), n = u32at(stream, at);
        if (uint64_t(at) + n > stream.size() || uint64_t(a) + n > mem->bytes.size()) return 7;   // cannot happen: sized from the same chunks
        if (memcmp(mem->bytes.data() + a, stream.data() + at, n) != 0) { memcpy(mem->bytes.data() + a, stream.data() + at, n); r->cache().invalidateRange(a, n); }
        at += n;
      } else if (type == 2) {
        u32at(stream, at);
        if (pass == 0 && frames == 0 && !meas) out("  step: first frame end chunk %u\n", unsigned(c));
        // skip=1: every other frame is skipped (setSkipNextFrame), never the last of a pass: its pixels must equal the unskipped run's
        if (ph.skip && (frameInPass % 2 == 0) && frameInPass + 1 < framesPerPass) r->setSkipNextFrame(true);
        frameInPass++;
        if (ph.rkeys && r->drawReplay()) { const u64 tk = svcGetSystemTick(); r->computeReplayKeys(); pending += ms(svcGetSystemTick() - tk); }   // (Claude v121) on the FIFO worker in the game: counted as process
        const uint32_t a0 = g_allocs; const u64 t = svcGetSystemTick();
        if (ph.ft) r->setFrameTiming(true);   // (Claude v122) render-split ticks
        const t2p::FrameReport rep = r->renderFrame(true);
        if (meas && rep.skipped) skippedFrames++;
        const u64 tw = svcGetSystemTick();
        C3Di_RenderQueueWaitDone();
        if (meas) gpuWait += ms(svcGetSystemTick() - tw);
        const double e = ms(svcGetSystemTick() - t);
        if (meas) {
          clipTests+=rep.clipTests;clipCulled+=rep.clipCulled;
          process += pending; render += e; build += rep.buildMs; plan += rep.planMs; draw += rep.drawMs;
          aProc += pendingA; aRender += g_allocs - a0; draws += rep.drawn; gx += rep.gxDraws; frames++; rpHits += rep.replayHits; rpMiss += rep.replayMisses; rpGroup += rep.replayGroupHits; rpStale += rep.replayStale; rpIns += rep.replayInserts; rsHit += rep.residentHits; rsMiss += rep.residentMisses; rsFull += rep.residentFull; rpRec += rep.replayRecorded;
          for (int k = 0; k < 8; k++) ftSplit[k] += rep.splitTicks[k]; for (int k = 0; k < 4; k++) ftMat[k] += rep.matrixSplit[k];
        }
        pending = 0; pendingA = 0;
      } else return 8;
    }
  }
  g_t2pProfOn = false;
  out("CLIP capture %s phase %s tests %.1f rejected %.1f per frame\n",capName.c_str(),ph.label.c_str(),double(clipTests)/std::max(frames,1u),double(clipCulled)/std::max(frames,1u));
  if (ph.retain) {
    const t2p::GxRetainStats rs = r->takeRetainStats();
    const double f = frames ? frames : 1;
    out("RETAIN capture %s phase %s per frame: draws %.0f skipped %.0f batches %.0f retained %.0f published %.0f materialized %.1f off %.1f overBudget %.1f verifyKB %.1f snapshotKB %.1f streamKB %.1f heldMB %.2f packMB %.2f packEntries %zu\n",
        capName.c_str(), ph.label.c_str(), rs.draws / f, rs.skipped / f, rs.batches / f, rs.retainedBatches / f, rs.published / f, rs.materialized / f, rs.closedOff / f,
        rs.overBudget / f, rs.verifyBytes / f / 1024, rs.snapshotBytes / f / 1024, rs.streamBytes / f / 1024, rs.heldBytes / 1048576.0, r->packCache().bytes() / 1048576.0, r->packCache().entries());
  }
  if (ph.prof) {
    std::string s = "PROF capture " + capName + " phase " + ph.label + " ms/frame:";
    static const char* kNames[16] = {"vdecode", "sink", "verts", "statedecode", "close", "append", "skipverts", "offer", "ccache", "vpack", "fillrest", "pl_shattr", "pl_unif", "pl_tex", "pl_rest", "arena"};
    char b[64];
    for (int i = 0; i < 16; i++) {
      if (kNames[i][0] == '-') continue;
      if (i == 2 || i == 6) snprintf(b, sizeof b, " %s %.0f", kNames[i], double(g_t2pProf[i]) / frames);
      else snprintf(b, sizeof b, " %s %.3f", kNames[i], ms(g_t2pProf[i]) / frames);
      s += b;
    }
    out("%s\n", s.c_str());
  }
  GSPGPU_InvalidateDataCache(target->frameBuf.colorBuf, 240u * 400u * 4u);
  uint32_t px = 2166136261u;
  const auto* img = static_cast<const uint8_t*>(target->frameBuf.colorBuf);
  for (size_t i = 0; i < 240u * 400u * 4u; i++) px = (px ^ img[i]) * 16777619u;
  if (!frames) frames = 1;
  out("RESULT capture %s phase %s frames %u draws %.1f gx %.1f process %.3f render %.3f total %.3f build %.3f plan %.3f draw %.3f "
      "allocs_process %.1f allocs_render %.1f pixels %08lx timing %d lean %d retain %d skipped %u gpuwait %.3f\n",
      capName.c_str(), ph.label.c_str(), frames, double(draws) / frames, double(gx) / frames, process / frames, render / frames,
      (process + render) / frames, build / frames, plan / frames, draw / frames, double(aProc) / frames, double(aRender) / frames,
      (unsigned long)px, ph.timing, ph.lean, ph.retain, skippedFrames, gpuWait / (frames ? frames : 1));
  if (ph.rprof) {
    const uint64_t* q = t2p::g_replayProf; const double h = q[5] ? double(q[5]) : 1.0, us = 1e6 / SYSCLOCK_ARM11;
    out("RPROF capture %s phase %s hits %.0f/frame us/hit: lookup %.2f remap %.2f textures %.2f repack %.2f backend %.2f [shader+attr %.2f uniforms %.2f texenv+tex %.2f effect %.2f updatectx %.2f drawcall %.2f] lookup: range %.2f tail %.2f drawkey %.2f\n",
        capName.c_str(), ph.label.c_str(), h / frames, q[0] * us / h, q[1] * us / h, q[2] * us / h, q[3] * us / h, q[4] * us / h, q[6] * us / h, q[7] * us / h, q[8] * us / h, q[9] * us / h, q[10] * us / h, q[11] * us / h, q[12] * us / h, q[13] * us / h, q[14] * us / h);
  }
  out("REPLAY capture %s phase %s hits %.1f (recorded %.1f, groups %.1f) misses %.1f per frame, %zu KB\n", capName.c_str(), ph.label.c_str(), double(rpHits) / frames, double(rpRec) / frames, double(rpGroup) / frames, double(rpMiss) / frames, r->drawReplayBytes() >> 10);
  { uint64_t w[4]; for (int k = 0; k < 4; k++) w[k] = t2p::g_replayWhy[k] - why0[k];
    out("REPLAYWHY capture %s phase %s per frame: stale %.1f inserts %.1f; misses no-entry %.1f tail %.1f draw-state %.1f 2d %.1f; resident hits %.1f misses %.1f full %.1f\n", capName.c_str(), ph.label.c_str(),
        double(rpStale) / frames, double(rpIns) / frames, double(w[0]) / frames, double(w[1]) / frames, double(w[2]) / frames, double(w[3]) / frames, double(rsHit) / frames, double(rsMiss) / frames, double(rsFull) / frames); }
  if (ph.mbprof) { auto& P = t2p::matrixbatch::prof(); std::string w;
    for (int k = 0; k < t2p::matrixbatch::MB_N; k++) if (P.why[k]) w += std::string(" ") + t2p::matrixbatch::kWhy[k] + "=" + std::to_string(P.why[k] / frames);
    out("MBPROF capture %s phase %s (sampled 1/8) per frame: cheap %u calls %.3f ms, nonPose %u calls %.3f ms, heavy %u calls %.3f ms; nonPose parts drawkey %.3f pixel %.3f tex %.3f view %.3f uniform %.3f; why%s\n", capName.c_str(), ph.label.c_str(),
        P.calls[0] / frames, ms(P.ticks[0]) / frames, P.calls[1] / frames, ms(P.ticks[1]) / frames, P.calls[2] / frames, ms(P.ticks[2]) / frames,
        ms(P.np[0]) / frames, ms(P.np[1]) / frames, ms(P.np[2]) / frames, ms(P.np[3]) / frames, ms(P.np[4]) / frames, w.c_str()); }
  if (ph.ft) out("SPLIT capture %s phase %s ms/frame: policy %.2f cache-begin %.2f loop %.2f copies %.2f end+recycle %.2f matrix-prep %.2f event-recycle %.2f pop %.2f; matrix key %.2f append %.2f variant %.2f\n", capName.c_str(), ph.label.c_str(),
      ms(ftSplit[0]) / frames, ms(ftSplit[1]) / frames, ms(ftSplit[2]) / frames, ms(ftSplit[3]) / frames, ms(ftSplit[4]) / frames, ms(ftSplit[5]) / frames, ms(ftSplit[6]) / frames, ms(ftSplit[7]) / frames,
      ms(ftMat[0]) / frames, ms(ftMat[1]) / frames, ms(ftMat[2]) / frames);
  C3Di_RenderQueueWaitDone();
  r->cache().invalidateRange(0, 0xffffffffu);
  r->shutdown();
  return 0;
}

int run() {
  gfxInitDefault(); gfxSetWide(false); consoleInit(GFX_BOTTOM, nullptr); osSetSpeedupEnable(true);
  g_log = fopen((std::string(kDir) + "results.txt").c_str(), "w");
  if (!C3D_Init(0x100000)) { out("C3D_Init failed\n"); return 1; }
  C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
  if (!target) return 2;
  C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
  std::vector<std::string> caps; std::vector<Phase> phases; int warm = 2, measured = 4;
  if (FILE* f = fopen((std::string(kDir) + "config.txt").c_str(), "r")) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
      char* tok = strtok(line, " \t\r\n");
      if (!tok || tok[0] == '#') continue;
      if (!strcmp(tok, "capture")) { if (char* n = strtok(nullptr, " \t\r\n")) caps.push_back(n); }
      else if (!strcmp(tok, "passes")) { char* a = strtok(nullptr, " \t\r\n"); char* b = strtok(nullptr, " \t\r\n"); if (a && b) { warm = atoi(a); measured = atoi(b); } }
      else if (!strcmp(tok, "phase")) {
        Phase p; if (char* n = strtok(nullptr, " \t\r\n")) p.label = n;
        while (char* kv = strtok(nullptr, " \t\r\n")) {
          char* eq = strchr(kv, '='); if (!eq) continue; *eq = 0; const int v = atoi(eq + 1);
          if (!strcmp(kv, "timing")) p.timing = v; else if (!strcmp(kv, "lean")) p.lean = v; else if (!strcmp(kv, "runs")) p.runs = v; else if (!strcmp(kv, "vplan")) p.vplan = v;
          else if (!strcmp(kv, "vdecode")) p.vdecode = v; else if (!strcmp(kv, "planreuse")) p.planreuse = v;
          else if (!strcmp(kv, "fogprep")) p.fogprep = v; else if (!strcmp(kv, "packed")) p.packed = v; else if (!strcmp(kv, "prof")) p.prof = v; else if (!strcmp(kv, "retain")) p.retain = v; else if (!strcmp(kv, "staterep")) p.staterep = v; else if (!strcmp(kv, "inplace")) p.inplace = v; else if (!strcmp(kv, "finrep")) p.finrep = v; else if (!strcmp(kv, "planrep")) p.planrep = v; else if (!strcmp(kv, "skip")) p.skip = v; else if (!strcmp(kv, "unifrep")) p.unifrep = v; else if (!strcmp(kv, "framerep")) p.framerep = v; else if (!strcmp(kv, "tevrep")) p.tevrep = v; else if (!strcmp(kv, "ccrep")) p.ccrep = v; else if (!strcmp(kv, "fullplanrep")) p.fullplanrep = v; else if (!strcmp(kv, "smemo")) p.smemo = v; else if (!strcmp(kv, "texrep")) p.texrep = v; else if (!strcmp(kv, "vdecrep")) p.vdecrep = v; else if (!strcmp(kv, "drawreplay")) p.drawreplay = v; else if (!strcmp(kv, "resident")) p.resident = v; else if (!strcmp(kv, "flushall")) p.flushall = v; else if (!strcmp(kv, "rprof")) p.rprof = v; else if (!strcmp(kv, "rigidclip")) p.rigidclip = v; else if (!strcmp(kv, "record")) p.record = v; else if (!strcmp(kv, "kick")) p.kick = v; else if (!strcmp(kv, "rkeys")) p.rkeys = v; else if (!strcmp(kv, "split")) p.split = v; else if (!strcmp(kv, "mbatch")) p.mbatch = v; else if (!strcmp(kv, "ft")) p.ft = v; else if (!strcmp(kv, "spool")) p.spool = v; else if (!strcmp(kv, "fa")) p.fa = v; else if (!strcmp(kv, "mbprof")) p.mbprof = v;
          else out("unknown key %s\n", kv);
        }
        phases.push_back(p);
      }
    }
    fclose(f);
  }
  if (caps.empty() || phases.empty()) { out("config.txt needs capture and phase lines\n"); return 3; }
  const std::vector<uint8_t> bundle = load(std::string(kDir) + "bundle.bin");
  out("START warm %d measured %d captures %u phases %u\n", warm, measured, unsigned(caps.size()), unsigned(phases.size()));
  for (const std::string& c : caps) {
    const std::vector<uint8_t> stream = load(std::string(kDir) + c + ".wwrp");
    if (stream.size() < 16 || memcmp(stream.data(), "WWRP", 4)) { out("bad capture %s\n", c.c_str()); continue; }
    for (const Phase& p : phases) {
      if (!aptMainLoop()) break;
      int e = 0;
      try { e = runPhase(p, c, stream, bundle, warm, measured, target); }
      catch (const std::exception& x) { out("EXCEPTION %s\n", x.what()); e = 99; }
      if (e) out("ERROR capture %s phase %s code %d\n", c.c_str(), p.label.c_str(), e);
    }
  }
  out("DONE\n");
  if (g_log) fclose(g_log);
  C3D_RenderTargetDelete(target); C3D_Fini(); gfxExit();
  return 0;
}
}  // namespace

int main() { return run(); }

// (Claude v117) the renderer's port log hook (gpu_3ds.cpp in the game): to the bench log
extern "C" void n3dsLog(const char* fmt, ...) {
  if (!g_log) return;
  va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fflush(g_log);
}
