#include <thread>
#include <atomic>
#include <unordered_map>
// benchframe (PC): the CPU side of Renderer3ds::renderFrame without the GPU, to profile where a frame's time goes.
//   GX bytes -> GxFifo (+ decodeState and batching, as Renderer3ds::onDraw) -> per draw: buildDraw (DrawCompileCache) -> buildPlan (shader cache, texture cache)
//   usage: benchframe replay.wwrp [bundle.bin] [repeat]
// The replay is played `repeat` times (default 2) so that the later passes show the steady state (every cache warm). EFB copies are not executed
// (their textures are read from guest memory instead); palette loads are applied.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bundle.h"
#include "c3dplan.h"
#include "draw.h"
#include "drawcache.h"
#include "gxbatch.h"
#include "gxretain.h"
#include "gxfifo.h"
#include "gxstate.h"
#include "submit.h"
#include "texcache.h"

static uint64_t g_planHash = 0;
using namespace t2p;
#include <new>
static size_t g_allocs = 0, g_allocBytes = 0;
// ALLOCSITES=build|plan: count the allocations of that phase by call stack (printed at exit; addresses for addr2line, link with -no-pie)
#include <execinfo.h>
#include <map>
// (Claude) incremental state decode test support (STATEINC)
static t2p::GxFifo* g_incFifo = nullptr;
static int g_incPrev = -1;
static unsigned long g_incChecked = 0, g_incUsed = 0, g_incBad = 0;
static bool sameDecodedState(const t2p::GxState& a, const t2p::GxState& b) {
  using namespace t2p;
  const GxDrawState &A = a.draw, &B = b.draw;
  const Options o;
  if (!sameDrawKey(A, o, B, o)) return false;
  if (A.indirect != B.indirect || A.importBlockers != B.importBlockers || A.numPosMtx != B.numPosMtx || A.projFar != B.projFar || A.texmapScaled != B.texmapScaled) return false;
  if (A.alpha.ref0 != B.alpha.ref0 || A.alpha.ref1 != B.alpha.ref1 || A.alpha.comp0 != B.alpha.comp0 || A.alpha.comp1 != B.alpha.comp1 || A.alpha.op != B.alpha.op) return false;
  if (A.blend.mode != B.blend.mode || A.blend.src != B.blend.src || A.blend.dst != B.blend.dst || A.blend.logic != B.blend.logic) return false;
  if (A.z.compare != B.z.compare || A.z.func != B.z.func || A.z.update != B.z.update) return false;
  if (A.fog.type != B.fog.type || A.fog.r != B.fog.r || A.fog.g != B.fog.g || A.fog.b != B.fog.b || std::memcmp(&A.fog.startZ, &B.fog.startZ, 4) || std::memcmp(&A.fog.endZ, &B.fog.endZ, 4) || std::memcmp(&A.fog.nearZ, &B.fog.nearZ, 4) || std::memcmp(&A.fog.farZ, &B.fog.farZ, 4)) return false;
  if (A.tev.stages.size() != B.tev.stages.size() || A.vtx.texgen.size() != B.vtx.texgen.size()) return false;
  {   // (Claude v112) the uniforms a draw reads: every non-pose row, pose rows below numPosMtx (compact blocks fill no others)
    const VtxUniformView u = a.uniformView(), v = b.uniformView(); const size_t np = size_t(A.numPosMtx);
    if (std::memcmp(u.proj, v.proj, sizeof(float) * 16) || std::memcmp(u.pos, v.pos, np * sizeof u.pos[0]) || std::memcmp(u.nrm, v.nrm, np * sizeof u.nrm[0]) ||
        std::memcmp(u.matColor, v.matColor, sizeof(float) * 4) || std::memcmp(u.ambColor, v.ambColor, sizeof(float) * 4) || std::memcmp(u.light, v.light, sizeof(VtxLight) * 8) ||
        std::memcmp(u.texMtx, v.texMtx, sizeof(float) * 120) || std::memcmp(u.texScale, v.texScale, sizeof(float) * 6)) return false;
  }
  if (std::memcmp(a.kcolor, b.kcolor, sizeof a.kcolor) || std::memcmp(a.tevReg, b.tevReg, sizeof a.tevReg)) return false;
  for (int m = 0; m < 8; m++) {
    const GxTexBinding &x = a.tex[m], &y = b.tex[m];
    if (x.valid != y.valid || x.addr != y.addr || x.width != y.width || x.height != y.height || x.fmt != y.fmt || x.wrapS != y.wrapS || x.wrapT != y.wrapT || x.magLinear != y.magLinear ||
        x.minLinear != y.minLinear || x.mipMode != y.mipMode || std::memcmp(&x.lodBias, &y.lodBias, 4) || std::memcmp(&x.minLod, &y.minLod, 4) || std::memcmp(&x.maxLod, &y.maxLod, 4) ||
        x.edgeLod != y.edgeLod || x.biasClamp != y.biasClamp || x.aniso != y.aniso || x.tlutOffset != y.tlutOffset || x.tlutFmt != y.tlutFmt) return false;
  }
  if (std::memcmp(&a.viewport, &b.viewport, sizeof a.viewport) || std::memcmp(&a.scissor, &b.scissor, sizeof a.scissor)) return false;
  if (a.proj.ortho != b.proj.ortho || std::memcmp(a.proj.p, b.proj.p, sizeof a.proj.p) || std::memcmp(&a.proj.nearZ, &b.proj.nearZ, 4) || std::memcmp(&a.proj.farZ, &b.proj.farZ, 4) || std::memcmp(a.proj.m, b.proj.m, sizeof a.proj.m)) return false;
  if (a.cullMode != b.cullMode || a.colorUpdate != b.colorUpdate || a.alphaUpdate != b.alphaUpdate || a.dither != b.dither || a.dstAlphaEnable != b.dstAlphaEnable || a.dstAlpha != b.dstAlpha ||
      a.pixelFmt != b.pixelFmt || a.zFmt16 != b.zFmt16 || a.zCompBeforeTex != b.zCompBeforeTex || a.numTexGens != b.numTexGens || a.numChans != b.numChans || a.numTevStages != b.numTevStages ||
      a.numIndStages != b.numIndStages || a.curPosMtx != b.curPosMtx || a.uniformPosSel != b.uniformPosSel || a.notes != b.notes) return false;
  return true;
}
static bool leanState() { static const bool on = std::getenv("LEANSTATE") != nullptr; return on; }   // as Renderer3ds::setStateNotes(false)
static int g_siteMode = 0;   // 1: recording
static std::map<std::vector<void*>, size_t>* g_sites = nullptr;
static void noteSite() {
  if (!g_siteMode) return;
  g_siteMode = 0;
  void* fr[10]; const int n = backtrace(fr, 10);
  std::vector<void*> key(fr + 2, fr + n);
  if (!g_sites) g_sites = new std::map<std::vector<void*>, size_t>;
  (*g_sites)[key]++;
  g_siteMode = 1;
}
void* operator new(size_t n) { g_allocs++; g_allocBytes += n; noteSite(); if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](size_t n) { g_allocs++; g_allocBytes += n; if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Mem1 : GuestMem {
  std::vector<uint8_t> bytes;
  const uint8_t* read(uint32_t addr, size_t size) const override {
    addr &= 0x03FFFFFF;
    if (size_t(addr) + size > bytes.size()) return nullptr;
    return &bytes[addr];
  }
};
struct SnapMem : GuestMem {
  uint32_t addr = 0;
  const std::vector<uint8_t>* bytes = nullptr;
  const uint8_t* read(uint32_t a, size_t n) const override {
    if (!bytes || a < addr || uint64_t(a) + n > uint64_t(addr) + bytes->size()) return nullptr;
    return bytes->data() + (a - addr);
  }
};

// RETAINSTAT=1: could each GX draw's decode be skipped because its inputs (stream bytes, format, bindings, referenced array bytes) equal a draw of the
// previous frame? And how many array bytes would proving that cost?
struct RetainStat {
  struct Rec { std::vector<std::pair<const uint8_t*, std::vector<uint8_t>>> snaps; };
  std::unordered_multimap<uint64_t, Rec> prev, cur;
  uint64_t draws = 0, verts = 0, hit = 0, hitVerts = 0, keyOnly = 0, slow = 0, streamB = 0, rangeB = 0, uniqB = 0, outB = 0, chunkUnionB = 0;
  std::unordered_map<const uint8_t*, std::pair<uint32_t, uint32_t>> chunkSeen;   // array base -> [lo, hi) bytes verified in this process() call
  uint64_t batches = 0, batchHit = 0, batchVerts = 0, batchHitVerts = 0;
  bool lastHit = false; uint32_t lastVerts = 0;
  static uint64_t fnv(const void* p, size_t n, uint64_t h) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } return h; }
  void chunk() { for (auto& kv : chunkSeen) chunkUnionB += kv.second.second - kv.second.first; chunkSeen.clear(); }
  void note(const GxDrawInputs& in) {
    draws++; verts += in.vertices; lastVerts = in.vertices;
    uint64_t h = fnv(&in.cmd, 1, 1469598103934665603ull);
    h = fnv(in.fmtRaw, 20, h); h = fnv(&in.vertices, 4, h); h = fnv(in.stream, size_t(in.vertices) * in.vsize, h);
    for (int p = 0; p < in.partCount; p++) { h = fnv(in.partPrim + p * in.partStride, 1, h); h = fnv(reinterpret_cast<const uint8_t*>(in.partCount32) + p * in.partStride, 4, h); }
    streamB += size_t(in.vertices) * in.vsize;
    Rec rec;
    for (int k = 0; k < in.nAttrs; k++) {
      const GxAttrPlan& a = in.attrs[k];
      switch (a.kind) { case GxAttrPlan::Byte: outB += in.vertices; break; case GxAttrPlan::Clr: outB += 4 * in.vertices; break; case GxAttrPlan::Tex: outB += 8 * in.vertices; break; default: outB += 12 * in.vertices; }
      if (a.kind == GxAttrPlan::Byte || a.type == GXA_DIRECT || a.type == GXA_NONE) continue;
      if (!a.base) { slow++; continue; }
      h = fnv(&a.base, sizeof a.base, h); h = fnv(&a.stride, 4, h); h = fnv(&a.size, 4, h); h = fnv(&a.be, 1, h);
      uint32_t lo = ~0u, hi = 0; std::vector<uint32_t> idx(in.vertices);
      for (uint32_t v = 0; v < in.vertices; v++) { const uint8_t* s = in.stream + size_t(v) * in.vsize + a.offset; const uint32_t i = a.type == GXA_INDEX16 ? (uint32_t(s[0]) << 8 | s[1]) : s[0]; idx[v] = i; lo = std::min(lo, i); hi = std::max(hi, i); }
      if (!in.vertices) continue;
      std::sort(idx.begin(), idx.end()); idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
      uniqB += idx.size() * a.need;
      const uint32_t b0 = lo * a.stride, b1 = std::min<uint32_t>(a.size, hi * a.stride + a.need);
      rangeB += b1 - b0;
      auto& cs = chunkSeen[a.base];
      if (cs.second == 0) cs = {b0, b1}; else { cs.first = std::min(cs.first, b0); cs.second = std::max(cs.second, b1); }
      rec.snaps.push_back({a.base + b0, std::vector<uint8_t>(a.base + b0, a.base + b1)});
    }
    bool ok = false; auto range = prev.equal_range(h);
    for (auto it = range.first; it != range.second && !ok; ++it) {
      bool same = it->second.snaps.size() == rec.snaps.size();
      for (size_t i = 0; same && i < rec.snaps.size(); i++) same = it->second.snaps[i].first == rec.snaps[i].first && it->second.snaps[i].second == rec.snaps[i].second;
      if (same) { ok = true; prev.erase(it); }
    }
    if (!ok && range.first != range.second) keyOnly++;
    if (ok) { hit++; hitVerts += in.vertices; }
    lastHit = ok;
    cur.emplace(h, std::move(rec));
  }
  bool openBatch = false, openAll = true; uint32_t openVerts = 0;
  void closeBatch() { if (openBatch) { batches++; batchVerts += openVerts; if (openAll) { batchHit++; batchHitVerts += openVerts; } } openBatch = false; }
  void drawInto(bool newBatch) { if (newBatch) { closeBatch(); openBatch = true; openAll = true; openVerts = 0; } openAll &= lastHit; openVerts += lastVerts; }
  void frameEnd() {
    closeBatch(); chunk();
    auto mb = [](uint64_t b) { return b / 1048576.0; };
    std::printf("   retain: draws %llu/%llu hit (%.1f%%) verts %.1f%% keyOnly %llu slow %llu | batches %llu/%llu fully hit (verts %.1f%%) | MB stream %.2f range %.2f uniq %.2f chunkUnion %.2f decodedOut %.2f\n",
                (unsigned long long)hit, (unsigned long long)draws, draws ? 100.0 * hit / draws : 0.0, verts ? 100.0 * hitVerts / verts : 0.0, (unsigned long long)keyOnly, (unsigned long long)slow,
                (unsigned long long)batchHit, (unsigned long long)batches, batchVerts ? 100.0 * batchHitVerts / batchVerts : 0.0, mb(streamB), mb(rangeB), mb(uniqB), mb(chunkUnionB), mb(outB));
    prev.swap(cur); cur.clear();
    draws = verts = hit = hitVerts = keyOnly = slow = streamB = rangeB = uniqB = outB = chunkUnionB = 0; batches = batchHit = batchVerts = batchHitVerts = 0;
  }
};
static RetainStat* g_retain = nullptr;

static bool retainOn() { static const bool on = std::getenv("RETAIN") && std::atoi(std::getenv("RETAIN")); return on; }
struct Ev { std::shared_ptr<const RetainedGeometry> geo; int maxPm = -1; bool tlut = false; GxState st; GxDraw d; GxTlutLoad tl; std::vector<uint8_t> tlutBytes; };

struct Sink : GxSink {
  Mem1* mem = nullptr;
  const GxFifo* fifo = nullptr;
  std::vector<Ev> evs;
  bool lastIsDraw = false;
  uint64_t lastGen = ~0ull;
  size_t gxDraws = 0;
  uint64_t drawHash = 1469598103934665603ull;   // DRAWHASH=1: FNV-1a over every decoded draw (checks that decoder changes keep the output)
  template <class T> void hv(const std::vector<T>& v) { hb(v.data(), v.size() * sizeof(T)); uint64_t n = v.size(); hb(&n, 8); }
  void hb(const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; i++) { drawHash ^= b[i]; drawHash *= 1099511628211ull; } }
  void hashDraw(const GxDraw& d) {
    const uint32_t hdr[3] = {d.prim, d.vatIdx, d.vertexCount};
    hb(hdr, sizeof hdr); hb(&d.v.count, 4);
    hv(d.v.pos); hv(d.v.nrm); hv(d.v.pmIdx); hv(d.tris);
    for (int c = 0; c < 2; c++) hv(d.v.clr[c]);
    for (int t = 0; t < 8; t++) { hv(d.v.tex[t]); hv(d.v.texMtxIdx[t]); }
  }
  // REUSE=1: is this GX draw's decoded geometry byte-identical to one in the previous frame?
  std::unordered_map<uint64_t, int> reusePrev, reuseCur;
  uint64_t reuseDraws = 0, reuseHit = 0, reuseVerts = 0, reuseHitVerts = 0;
  void reuseNote(const GxDraw& d) {
    const uint64_t keep = drawHash; drawHash = 1469598103934665603ull; hashDraw(d); const uint64_t h = drawHash; drawHash = keep;
    reuseDraws++; reuseVerts += d.v.count;
    auto it = reusePrev.find(h);
    if (it != reusePrev.end() && it->second > 0) { it->second--; reuseHit++; reuseHitVerts += d.v.count; }
    reuseCur[h]++;
  }
  void reuseFrameEnd() {
    std::printf("   reuse: %llu/%llu GX draws (%.1f%%), %llu/%llu vertices (%.1f%%) identical to a draw of the previous frame\n",
                (unsigned long long)reuseHit, (unsigned long long)reuseDraws, reuseDraws ? 100.0 * reuseHit / reuseDraws : 0.0,
                (unsigned long long)reuseHitVerts, (unsigned long long)reuseVerts, reuseVerts ? 100.0 * reuseHitVerts / reuseVerts : 0.0);
    reusePrev.swap(reuseCur); reuseCur.clear(); reuseDraws = reuseHit = reuseVerts = reuseHitVerts = 0;
  }
  void onDraw(const GxRegs& r, const GxDraw& d) override {
    gxDraws++;
    static const bool doReuse = std::getenv("REUSE") != nullptr;
    if (doReuse) reuseNote(d);
    static const bool doHash = std::getenv("DRAWHASH") != nullptr;
    if (doHash) hashDraw(d);
    const uint64_t gen = fifo->stateGeneration();
    if (lastIsDraw && gen == lastGen && canAppend(evs.back().d, d)) {
      if (g_retain) g_retain->drawInto(false);
      Ev& x = evs.back();
      const int before = maxPosMtx(x.d);
      appendDraw(x.d, d);
      if (maxPosMtx(d) > before) x.st = decodeState(r, &x.d, !leanState());
      return;
    }
    if (g_retain) g_retain->drawInto(true);
    evs.emplace_back();
    evs.back().d = d;
    evs.back().st = decodeState(r, &d, !leanState());
    lastIsDraw = true; lastGen = gen;
  }
  // APPENDPATH=1: decode in place like Renderer3ds (appendTarget / onAppended, renderer3ds.cpp), the path the 3DS runs; default: onDraw (copying)
  bool appendNew = false;
  int lastMaxPm = -1;
  std::vector<GxDraw> pool;
  GxRetainer retainer;
  bool skipped = false; int skipMaxPm = -1;
  void closeRetained() {
    if (!retainer.open()) return;
    if (!evs.empty() && !evs.back().tlut) { Ev& x = evs.back(); if (auto g = retainer.closeBatch(x.d, x.maxPm)) x.geo = std::move(g); }
    else { GxDraw none; retainer.closeBatch(none, -1); }
  }
  bool retainOffer(const GxDrawInputs& in, GxDraw& D) override { int m = -1; skipped = retainer.offer(in, D, appendNew, fifo->processSerial(), m); skipMaxPm = m; return skipped; }
  void retainDecoded(const GxDrawInputs& in, const GxDraw& D, uint32_t firstVertex, size_t firstTri, bool arrayError) override { retainer.decoded(in, D, firstVertex, firstTri, arrayError, fifo->processSerial()); }
  GxDraw* appendTarget(const GxRegs&, const GxDraw& h) override {
    static const bool on = std::getenv("APPENDPATH") != nullptr;
    if (!on) return nullptr;
    gxDraws++;
    const uint64_t gen = fifo->stateGeneration();
    if (lastIsDraw && gen == lastGen && canAppendHeader(evs.back().d, h, kBatchMaxVertices)) { appendNew = false; return &evs.back().d; }
    closeRetained();
    evs.emplace_back();
    Ev& x = evs.back();
    if (!pool.empty()) { x.d = std::move(pool.back()); pool.pop_back(); }
    x.d.commandCount = h.commandCount; x.d.prim = h.prim; x.d.vatIdx = h.vatIdx; x.d.vertexCount = h.vertexCount; x.d.unsupportedPrim = h.unsupportedPrim; x.d.vcd = h.vcd; x.d.vat = h.vat;
    x.d.retained = false; x.d.retainedTris = 0;
    appendNew = true; lastIsDraw = true; lastGen = gen;
    return &x.d;
  }
  void onAppended(const GxRegs& r, GxDraw& into, uint32_t firstVertex) override {
    const int mine = skipped ? skipMaxPm : maxPosMtxFrom(into, firstVertex);
    skipped = false;
    Ev& x = evs.back();
    const int known = retainOn() ? 0 : 1;
    // (Claude) STATEINC=1: the incremental state decode (gxstate.h decodeStateIncremental) with the fifo's dirty groups, as the 3DS recorder;
    // STATEINC=2: every incremental state is also decoded in full and compared field by field (mismatches counted, printed at exit).
    static const int stateInc = std::getenv("STATEINC") ? std::atoi(std::getenv("STATEINC")) : 0;
    auto decodeInc = [&](Ev& e) {
      const int kmp = known ? kScanPosMtx : e.maxPm;
      const uint32_t dirty = g_incFifo ? g_incFifo->takeStateDirty() : kStateDirtyAll;
      const GxState* prev = (stateInc && g_incPrev >= 0 && size_t(g_incPrev) < evs.size() && &evs[size_t(g_incPrev)] != &e) ? &evs[size_t(g_incPrev)].st : nullptr;
      if (!stateInc) { e.st = decodeState(r, &e.d, !leanState(), kmp); }
      else {
        GxState n;
        decodeStateIncremental(n, r, &e.d, !leanState(), kmp, dirty, prev);
        if (stateInc == 2) { const GxState full = decodeState(r, &e.d, !leanState(), kmp); g_incChecked++; if (prev) g_incUsed++; if (!sameDecodedState(n, full)) g_incBad++; }
        e.st = std::move(n);
      }
      g_incPrev = int(&e - evs.data());
    };
    if (appendNew) { x.maxPm = mine; decodeInc(x); }
    else if (mine > x.maxPm) { x.maxPm = mine; decodeInc(x); }
  }
  void onCopy(const GxRegs& r) override { closeRetained(); lastIsDraw = false; if (retainOn() && decodeCopy(r).toXfb) retainer.endFrame(); }
  void onTlutLoad(const GxRegs& r) override {
    closeRetained();
    lastIsDraw = false;
    evs.emplace_back();
    Ev& x = evs.back();
    x.tlut = true;
    x.tl = decodeTlutLoad(r);
    const size_t n = size_t(x.tl.entries) * 2;
    if (const uint8_t* s = n ? mem->read(x.tl.srcAddr, n) : nullptr) x.tlutBytes.assign(s, s + n);
  }
  const uint8_t* resolveDisplayList(uint32_t addr, uint32_t size) override { return mem->read(addr, size); }
};

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s replay.wwrp [bundle.bin] [repeat]\n", argv[0]); return 2; }
  const int repeat = argc > 3 ? std::atoi(argv[3]) : 2;
  if (std::getenv("SECTIONS")) t2p::g_uniformSections = true;   // (Claude v112) the 3DS compact uniform blocks (needs -DT2P_TEST_SHARED_UNIFORMS)
  if (std::getenv("VMAJOR")) t2p::g_packVertexMajor = true;   // (Claude) vertex-major packing
  std::vector<uint8_t> f;
  { FILE* in = std::fopen(argv[1], "rb"); if (!in) { std::perror(argv[1]); return 1; }
    uint8_t b[1 << 16]; size_t n; while ((n = std::fread(b, 1, sizeof b, in)) > 0) f.insert(f.end(), b, b + n); std::fclose(in); }
  MaterialBundle bundle;
  if (argc > 2 && argv[2][0]) {
    FILE* bf = std::fopen(argv[2], "rb"); std::vector<uint8_t> b;
    if (bf) { uint8_t t[1 << 14]; size_t k; while ((k = std::fread(t, 1, sizeof t, bf)) > 0) b.insert(b.end(), t, t + k); std::fclose(bf); }
    std::string err;
    if (bundle.load(b.data(), b.size(), sourceFingerprint(), &err)) setBundleLookup(&bundle);
    else std::printf("bundle not loaded: %s\n", err.c_str());
  }
  uint32_t hdr[3]; std::memcpy(hdr, &f[4], 12);
  Mem1 mem; mem.bytes.assign(hdr[1], 0);
  Sink sink; sink.mem = &mem;
  GxFifo fifo(&sink);
  g_incFifo = &fifo;
  struct IncReport { ~IncReport() { if (std::getenv("STATEINC")) std::fprintf(stderr, "stateinc: %lu states checked, %lu incremental, %lu MISMATCHES\n", g_incChecked, g_incUsed, g_incBad); } } incReport;
  sink.fifo = &fifo;
  if (retainOn()) fifo.setRetainOffers(true);
  PackedGeometryCache packCache;
  uint64_t frameHash = 0;   // FRAMEHASH=1: per frame, over every draw event's geometry, packed buffers and plan
  // (Claude) DEFER=1: deferred vertex decode with a real helper thread (gxfifo.h setDeferredDecode); DEFER=2: no helper (the processing thread drains)
  static std::atomic<bool> g_deferStop{false};
  std::thread deferHelper;
  if (const char* dv = std::getenv("DEFER")) { fifo.setDeferredDecode(true); if (std::atoi(dv) == 1) deferHelper = std::thread([&fifo] { while (!g_deferStop.load()) { if (!fifo.runDecodeJob()) std::this_thread::yield(); } }); }
  struct DeferJoin { std::thread& t; GxFifo& f; ~DeferJoin() { if (t.joinable()) { g_deferStop = true; t.join(); } std::fprintf(stderr, "deferred decode jobs %llu\n", (unsigned long long)f.deferredJobs()); } } deferJoin{deferHelper, fifo};
  if (std::getenv("RETAINSTAT")) { g_retain = new RetainStat; fifo.setInputObserver([](const GxDrawInputs& in) { g_retain->note(in); }); }
  fifo.setGuestArrayReader([&mem](uint32_t a, uint32_t n) { return mem.read(a, n); });
  fifo.setAuroraMode(hdr[0] == 2);   // version 2: a stream from Aurora (the native port), pc_wwrp_capture.cpp

  Options base; base.verifySamples = 0; base.approxEffort = 0; base.packedUniforms = std::getenv("PACKED") != nullptr;   // RendererOptions::translate
  FrameConfig fc; fc.efbW = 640; fc.efbH = 480; fc.targetW = 400; fc.targetH = 240; fc.fit = FitMode::Pillarbox43; fc.rotate = true; fc.resolve();
  DrawCompileCache drawCache(4096);
  C3dShaderCache shaders;
  TexCacheOptions to; to.tryEtc1 = std::getenv("ETC1") != nullptr; to.sampleOncePerFrame = true;   // as Renderer3ds unless ETC1=1
  TextureCache tex(to);

  for (int pass = 0; pass < repeat; pass++) {
    size_t at = 16;
    double tProcess = 0; size_t aProcess = 0, aBuild = 0, aPlan = 0;
    for (uint32_t i = 0; i < hdr[2]; i++) {
      const uint8_t type = f[at]; at += 4;
      uint32_t a = 0, n = 0;
      if (type == 0) {
        std::memcpy(&n, &f[at], 4); at += 4;
        const auto t0 = Clock::now(); const size_t a0 = g_allocs; { static const char* st = std::getenv("ALLOCSITES"); g_siteMode = st && !std::strcmp(st, "process") && pass == repeat - 1; } fifo.process(&f[at], n); fifo.finishDecodes(); if (g_retain) g_retain->chunk(); g_siteMode = 0; tProcess += ms(t0, Clock::now()); aProcess += g_allocs - a0;
        at += n;
      } else if (type == 1) {
        std::memcpy(&a, &f[at], 4); std::memcpy(&n, &f[at + 4], 4); at += 8; std::memcpy(&mem.bytes[a], &f[at], n); at += n;
        tex.invalidateRange(a, n);
      } else {
        uint32_t fr; std::memcpy(&fr, &f[at], 4); at += 4;
        sink.closeRetained();
        const auto t0 = Clock::now();
        std::vector<GxDrawState> states;
        for (auto& e : sink.evs) if (!e.tlut) states.push_back(e.st.draw);
        const auto tS = Clock::now();
        Options opt = base; opt.frameFogRange = frameFogRange(states.data(), states.size());
        const auto tF = Clock::now();
        tex.beginFrame(mem);
        const auto tB = Clock::now();
        double tBuild = 0, tPlan = 0, tLoop = 0; int draws = 0, ok = 0;
        const auto tL0 = Clock::now();
        const TexCacheStats c0 = tex.stats();
        for (Ev& e : sink.evs) {
          if (e.tlut) { SnapMem s; s.addr = e.tl.srcAddr; s.bytes = &e.tlutBytes; tex.tlut().load(e.tl, s); continue; }
          static const char* sites = std::getenv("ALLOCSITES");
          const bool last = pass == repeat - 1;
          const auto a0 = Clock::now(); const size_t n0 = g_allocs;
          g_siteMode = last && sites && !std::strcmp(sites, "build");
          // PACKETCHECK=1: the reused-packet path (buildDrawInto, notes off: what the 3DS renderer runs) must give the packet buildDraw gives
          static const bool check = std::getenv("PACKETCHECK") != nullptr;
          static DrawPacket reused;
          const GxDraw& G = e.geo ? e.geo->d : e.d;
          buildDrawInto(reused, e.st, G, opt, fc, &drawCache, false, e.geo ? &packCache : nullptr, e.geo ? e.geo->id : 0);
          g_siteMode = 0;
          const DrawPacket& pk = reused;
          if (check) {
            const DrawPacket ref = buildDraw(e.st, G, opt, fc, &drawCache);
            auto same = [](const auto& a, const auto& b) { return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0]))); };
            bool eq = ref.ok == pk.ok && ref.stride == pk.stride && ref.vertexCount == pk.vertexCount && same(ref.vertices, pk.vertices) && same(ref.indices, pk.indices) &&
                      ref.texenvUpdateBuffer == pk.texenvUpdateBuffer && same(ref.texenv, pk.texenv) && ref.layout.size() == pk.layout.size() && !!ref.second == !!pk.second &&
                      ref.uniforms.size() == pk.uniforms.size() && &ref.pica() == &pk.pica() && ref.lodFrameBias == pk.lodFrameBias;
            for (size_t i = 0; eq && i < ref.layout.size(); i++) eq = ref.layout[i].offset == pk.layout[i].offset && ref.layout[i].reg == pk.layout[i].reg && ref.layout[i].kind == pk.layout[i].kind;
            for (auto& kv : ref.uniforms) { auto it = pk.uniforms.find(kv.first); if (eq && (it == pk.uniforms.end() || !same(kv.second, it->second))) eq = false; }
            for (int u = 0; eq && u < 3; u++) eq = ref.units[u].texmap == pk.units[u].texmap && ref.units[u].scaleS == pk.units[u].scaleS && ref.units[u].scaleT == pk.units[u].scaleT;
            static size_t checked = 0, bad = 0;
            checked++; if (!eq) bad++;
            if (!eq && bad < 5) {
              std::printf("PACKET MISMATCH at draw %d: ok %d/%d stride %u/%u verts %zu/%zu idx %zu/%zu texenv %zu/%zu unif %zu/%zu second %d/%d pica %d lod %g/%g\n", draws, ref.ok, pk.ok, ref.stride, pk.stride,
                          ref.vertices.size(), pk.vertices.size(), ref.indices.size(), pk.indices.size(), ref.texenv.size(), pk.texenv.size(), ref.uniforms.size(), pk.uniforms.size(),
                          !!ref.second, !!pk.second, &ref.pica() == &pk.pica(), ref.lodFrameBias, pk.lodFrameBias);
              for (auto& kv : ref.uniforms) { auto it = pk.uniforms.find(kv.first); if (it == pk.uniforms.end() || !same(kv.second, it->second)) std::printf("   uniform %s differs\n", kv.first.c_str()); }
              for (auto& kv : pk.uniforms) if (!ref.uniforms.count(kv.first)) std::printf("   uniform %s only in reused (%zu)\n", kv.first.c_str(), kv.second.size());
            }
            if (checked % 500 == 0) std::printf("packet check: %zu draws, %zu mismatches\n", checked, bad);
          }
          const auto a1 = Clock::now(); const size_t n1 = g_allocs;
          g_siteMode = last && sites && !std::strcmp(sites, "plan");
          static const bool fh = std::getenv("FRAMEHASH") != nullptr;
          if (fh) {
            auto mix = [&](const void* p, size_t n) { frameHash = hashBytes(static_cast<const uint8_t*>(p), n, frameHash); };
            const uint64_t keep = sink.drawHash; sink.drawHash = frameHash; sink.hashDraw(G); frameHash = sink.drawHash; sink.drawHash = keep;
            mix(&e.st.draw.numPosMtx, sizeof e.st.draw.numPosMtx); mix(&pk.ok, 1); mix(&pk.stride, 4); mix(&pk.vertexCount, 4);
            mix(pk.vertices.data(), pk.vertices.size()); mix(pk.indices.data(), pk.indices.size() * 2);
            for (const SubmitAttr& sa : pk.layout) { mix(&sa.kind, sizeof sa.kind); mix(&sa.reg, 4); mix(&sa.offset, 4); }
          }
          bool planOkForRelease = false;
          if (pk.ok && !pk.second) { static const int planRep = std::getenv("PLANREPEAT") ? std::atoi(std::getenv("PLANREPEAT")) : 1; for (int r = 1; r < planRep; r++) buildPlan(pk, shaders, tex, mem); C3dPlan L = buildPlan(pk, shaders, tex, mem); ok += L.ok; planOkForRelease = L.ok;
            if (fh) { frameHash = hashBytes(reinterpret_cast<const uint8_t*>(&L.ok), 1, frameHash); if (L.ok) { frameHash = hashBytes(reinterpret_cast<const uint8_t*>(&L.hashAttrs), 8, frameHash); for (const C3dUniform& u : L.uniforms) frameHash = hashBytes(reinterpret_cast<const uint8_t*>(u.rows), sizeof(PVec) * size_t(u.count), frameHash); } }
            // PLANHASH=1: everything the executor reads from the plan (texture entries by id), to compare planner changes
            static const bool ph = std::getenv("PLANHASH") != nullptr;
            if (ph) {
              auto mix = [](const void* p, size_t n) { g_planHash = hashBytes(static_cast<const uint8_t*>(p), n, g_planHash); };
              mix(&L.ok, 1); if (L.ok) {
                mix(&L.shader->key, 8); mix(&L.hashAttrs, 8); mix(&L.hashTexEnv, 8); mix(&L.hashEffect, 8); mix(&L.attrs.stride, sizeof L.attrs.stride);
                mix(&L.vertexBytes, 4); mix(&L.indexCount, 4); mix(&L.scissor, sizeof L.scissor);
                for (const C3dUniform& u : L.uniforms) { mix(u.name->data(), u.name->size()); mix(&u.id, 4); mix(&u.count, 4); mix(u.rows, sizeof(PVec) * size_t(u.count)); }
                for (const C3dUnit& U : L.units) { mix(&U.enabled, 1); if (!U.enabled) continue; mix(&U.entry->id, sizeof U.entry->id); mix(&U.projective, 1); mix(&U.wrapS, 1); mix(&U.wrapT, 1); mix(&U.magFilter, 3); mix(&U.minLevel, 2); mix(&U.lodBias, 2); }
                for (const std::string& n : L.notes) mix(n.data(), n.size());
              }
            }
          }
          g_siteMode = 0;
          if (reused.packedFrom || reused.packKey) releasePacked(reused, packCache, planOkForRelease);
          aBuild += n1 - n0; aPlan += g_allocs - n1;
          const auto a2 = Clock::now();
          tBuild += ms(a0, a1); tPlan += ms(a1, a2); draws++;
        }
        tLoop = ms(tL0, Clock::now());
        std::printf("   states copy %.1f ms  frameFogRange %.1f ms  tex.beginFrame %.1f ms  draw loop %.1f ms\n", ms(t0, tS), ms(tS, tF), ms(tF, tB), tLoop);
        const TexCacheStats c1 = tex.stats();
        std::printf("   allocations: process %zu  build %zu  plan %zu  (per draw: build %.0f plan %.0f)\n", aProcess, aBuild, aPlan, double(aBuild) / draws, double(aPlan) / draws);
        aProcess = aBuild = aPlan = 0;
        std::printf("   texture cache: %zu entries, %.2f MB converted\n", c1.entries, c1.bytes / 1048576.0);
        { static const bool doReuse = std::getenv("REUSE") != nullptr; if (doReuse) sink.reuseFrameEnd(); }
        if (g_retain) g_retain->frameEnd();
        if (retainOn()) { packCache.endFrame(); const GxRetainStats rs = sink.retainer.takeStats();
          std::printf("   retainer: draws %llu skipped %llu | batches %llu retained %llu published %llu materialized %llu notRetainable %llu off %llu overBudget %llu | verify %.2f MB snapshot %.2f MB stream %.2f MB | held %.2f MB pack %.2f MB (%zu)\n",
            (unsigned long long)rs.draws, (unsigned long long)rs.skipped, (unsigned long long)rs.batches, (unsigned long long)rs.retainedBatches, (unsigned long long)rs.published,
            (unsigned long long)rs.materialized, (unsigned long long)rs.notRetainable, (unsigned long long)rs.closedOff, (unsigned long long)rs.overBudget, rs.verifyBytes / 1048576.0, rs.snapshotBytes / 1048576.0, rs.streamBytes / 1048576.0,
            rs.heldBytes / 1048576.0, packCache.bytes() / 1048576.0, packCache.entries()); }
        if (std::getenv("FRAMEHASH")) { std::printf("   framehash %016llx\n", (unsigned long long)frameHash); frameHash = 0; }
        std::printf("pass %d frame %u: gx draws %zu -> %d draws (%d planned)  process %.1f ms  build %.1f ms  plan %.1f ms  (texture conversions %llu)  total %.1f ms\n", pass, fr,
                    sink.gxDraws, draws, ok, tProcess, tBuild, tPlan, (unsigned long long)(c1.conversions - c0.conversions), ms(t0, Clock::now()) + tProcess);
        for (Ev& e : sink.evs) {   // as Renderer3ds: emptied draws keep their capacity for the next frame
          if (e.tlut) continue;
          GxVertices& W = e.d.v; W.count = 0; W.pos.clear(); W.nrm.clear(); W.pmIdx.clear();
          for (int c = 0; c < 2; c++) W.clr[c].clear();
          for (int t = 0; t < 8; t++) { W.tex[t].clear(); W.texMtxIdx[t].clear(); }
          e.d.tris.clear();
          if (sink.pool.size() < 1024) sink.pool.push_back(std::move(e.d));
        }
        sink.evs.clear(); g_incPrev = -1; sink.lastIsDraw = false; sink.gxDraws = 0; tProcess = 0;
      }
    }
  }
  if (g_sites) {
    std::vector<std::pair<size_t, const std::vector<void*>*>> v;
    for (auto& kv : *g_sites) v.push_back({kv.second, &kv.first});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < v.size() && i < 30; i++) { std::printf("SITE %zu", v[i].first); for (void* a : *v[i].second) std::printf(" %p", a); std::printf("\n"); }
  }
  if (std::getenv("DRAWHASH")) std::printf("draw hash %016llx\n", (unsigned long long)sink.drawHash);
  if (std::getenv("PLANHASH")) std::printf("plan hash %016llx\n", (unsigned long long)g_planHash);
  return 0;
}
