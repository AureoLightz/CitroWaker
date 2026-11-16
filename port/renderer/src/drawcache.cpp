#include "drawcache.h"

#include <cstdio>
#include <cstring>

namespace t2p {
namespace {

// Mixes a word at a time (the key has hundreds of small fields; byte-wise FNV made ~8 64-bit multiplies per field, slow on the ARM11). The hash is only
// an in-memory bucket index: a hit is always confirmed with sameDrawKey, so it may change freely.
// (Claude v77) 32-bit mixing per field: a 64x64 multiply per field was ~10 us per key on the ARM11 (several hundred fields); the bucket hash only
// needs to spread keys, every hit is still confirmed by sameDrawKey.
struct Hasher {
  uint32_t a = 0x811C9DC5u, b = 0x9E3779B9u;
  uint64_t h = 0;
  bool strong = false;   // (Claude v122) the 64-bit lane only for replay keys: on every compile-cache lookup it doubled the hash cost on the ARM11
  void u32(uint32_t v) { a = (a ^ v) * 0x01000193u; b = (b + v) * 0x85EBCA6Bu; b ^= b >> 15; if (strong) { h = (h ^ v) * 0x9E3779B97F4A7C15ull; h ^= h >> 31; } }   // h: (Claude v121) strong lane (replay keys)
  void u(uint64_t v) { u32(uint32_t(v)); if (v >> 32) u32(uint32_t(v >> 32)); }
  void bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; std::memcpy(&w, b, 8); u(w); }
    if (n) { uint64_t w = 0; std::memcpy(&w, b, n); u(w ^ (uint64_t(n) << 56)); }
  }
  void f(float v) { uint32_t x; std::memcpy(&x, &v, 4); u(x); }   // bit pattern: -0 and +0, NaNs differ (equal states hash equal, which is all that is needed)
  void d(double v) { uint64_t x; std::memcpy(&x, &v, 8); u(x); }
  void s(const std::string& v) { u(v.size()); bytes(v.data(), v.size()); }
};

void feed(Hasher& H, const TevChan& c) {
  for (Leaf l : c.in) H.u(l);
  H.u(c.sub); H.f(c.bias); H.f(c.scale); H.u(c.clamp); H.u(c.out); H.u(c.cmp);
}
void feed(Hasher& H, const KSel& k) { H.u(k.kind); H.f(k.fixed); H.u(k.idx); H.u(k.chan); }
void feed(Hasher& H, const TevStage& s) {
  feed(H, s.c); feed(H, s.a); feed(H, s.kc); feed(H, s.ka);
  H.u(uint64_t(int64_t(s.texmap))); H.u(uint64_t(int64_t(s.rasChan))); H.u(uint64_t(int64_t(s.texcoord)));
  for (uint8_t v : s.texSwap) H.u(v);
  for (uint8_t v : s.rasSwap) H.u(v);
  for (bool v : s.regBufC) H.u(v);
  for (bool v : s.regBufA) H.u(v);
}
void feed(Hasher& H, const VtxChan& c) { H.u(c.enable); H.u(c.matSrc); H.u(c.lightMask); H.u(c.diffuse); H.u(c.attn); H.u(c.ambSrc); }
void feed(Hasher& H, const VtxTexGen& t) { H.u(t.type); H.u(t.src); H.u(t.mtx); H.u(t.normalize); H.u(t.postMtx); }
void feed(Hasher& H, const VtxSetup& v) {
  feed(H, v.color0); feed(H, v.alpha0);
  H.u(v.needColor);
  H.u(v.texgen.size());
  for (const VtxTexGen& t : v.texgen) feed(H, t);
  H.u(uint64_t(int64_t(v.numPosMtx)));
  H.u(uint64_t(int64_t(v.inPos))); H.u(uint64_t(int64_t(v.inNrm))); H.u(uint64_t(int64_t(v.inClr0))); H.u(uint64_t(int64_t(v.inMtxIdx)));
  for (int i : v.inTex) H.u(uint64_t(int64_t(i)));
  H.u(v.flipT); H.u(v.texScaleMask);
  H.u(uint64_t(int64_t(v.dbgClipReg))); H.u(uint64_t(int64_t(v.dbgTc)));
}
void feed(Hasher& H, const GxDrawState& g) {
  H.u(g.tev.stages.size());
  for (const TevStage& s : g.tev.stages) feed(H, s);
  H.u(g.indirect);
  H.u(g.importBlockers.size());
  for (const std::string& b : g.importBlockers) H.s(b);
  feed(H, g.vtx);
  H.u(uint64_t(int64_t(g.numPosMtx)));
  H.u(g.alpha.comp0); H.u(g.alpha.ref0); H.u(g.alpha.op); H.u(g.alpha.comp1); H.u(g.alpha.ref1);
  H.u(g.blend.mode); H.u(g.blend.src); H.u(g.blend.dst); H.u(g.blend.logic);
  H.u(g.z.compare); H.u(g.z.func); H.u(g.z.update);
  H.u(g.fog.type); H.f(g.fog.startZ); H.f(g.fog.endZ); H.f(g.fog.nearZ); H.f(g.fog.farZ); H.u(g.fog.r); H.u(g.fog.g); H.u(g.fog.b);
  H.f(g.projFar);
  H.u(g.texmapScaled);
}
void feed(Hasher& H, const Options& o) {
  H.d(o.approxLimit); H.u(uint64_t(int64_t(o.verifySamples))); H.u(o.seed); H.f(o.frameFogRange); H.u(o.flipTexT); H.u(o.multipass); H.u(o.approximate); H.u(uint64_t(int64_t(o.approxEffort)));
}

// field-wise equality (floats by bit pattern, like the hash)
bool fbits(float a, float b) { return std::memcmp(&a, &b, 4) == 0; }
bool eq(const TevChan& a, const TevChan& b) {
  for (int i = 0; i < 4; i++) if (a.in[i] != b.in[i]) return false;
  return a.sub == b.sub && fbits(a.bias, b.bias) && fbits(a.scale, b.scale) && a.clamp == b.clamp && a.out == b.out && a.cmp == b.cmp;
}
bool eq(const KSel& a, const KSel& b) { return a.kind == b.kind && fbits(a.fixed, b.fixed) && a.idx == b.idx && a.chan == b.chan; }
bool eq(const TevStage& a, const TevStage& b) {
  if (!eq(a.c, b.c) || !eq(a.a, b.a) || !eq(a.kc, b.kc) || !eq(a.ka, b.ka)) return false;
  if (a.texmap != b.texmap || a.rasChan != b.rasChan || a.texcoord != b.texcoord) return false;
  for (int i = 0; i < 4; i++) if (a.texSwap[i] != b.texSwap[i] || a.rasSwap[i] != b.rasSwap[i]) return false;
  for (int i = 0; i < 3; i++) if (a.regBufC[i] != b.regBufC[i] || a.regBufA[i] != b.regBufA[i]) return false;
  return true;
}
bool eq(const VtxChan& a, const VtxChan& b) { return a.enable == b.enable && a.matSrc == b.matSrc && a.lightMask == b.lightMask && a.diffuse == b.diffuse && a.attn == b.attn && a.ambSrc == b.ambSrc; }
bool eq(const VtxTexGen& a, const VtxTexGen& b) { return a.type == b.type && a.src == b.src && a.mtx == b.mtx && a.normalize == b.normalize && a.postMtx == b.postMtx; }
bool eq(const VtxSetup& a, const VtxSetup& b, bool ignorePoseCount=false) {
  if (!eq(a.color0, b.color0) || !eq(a.alpha0, b.alpha0) || a.needColor != b.needColor || a.texgen.size() != b.texgen.size()) return false;
  if(!sameMaterialVectorOwner(a.texgen,b.texgen))
    for (size_t i = 0; i < a.texgen.size(); i++) if (!eq(a.texgen[i], b.texgen[i])) return false;
  if ((!ignorePoseCount && a.numPosMtx != b.numPosMtx) || a.inPos != b.inPos || a.inNrm != b.inNrm || a.inClr0 != b.inClr0 || a.inMtxIdx != b.inMtxIdx) return false;
  for (int i = 0; i < 8; i++) if (a.inTex[i] != b.inTex[i]) return false;
  return a.flipT == b.flipT && a.texScaleMask == b.texScaleMask && a.dbgClipReg == b.dbgClipReg && a.dbgTc == b.dbgTc;
}
bool eq(const GxDrawState& a, const GxDrawState& b, bool ignorePoseCount=false) {
  if (a.tev.stages.size() != b.tev.stages.size()) return false;
  if(!sameMaterialVectorOwner(a.tev.stages,b.tev.stages))
    for (size_t i = 0; i < a.tev.stages.size(); i++) if (!eq(a.tev.stages[i], b.tev.stages[i])) return false;
  if (a.indirect != b.indirect || a.importBlockers != b.importBlockers || !eq(a.vtx, b.vtx, ignorePoseCount) || (!ignorePoseCount && a.numPosMtx != b.numPosMtx)) return false;
  if (a.alpha.comp0 != b.alpha.comp0 || a.alpha.ref0 != b.alpha.ref0 || a.alpha.op != b.alpha.op || a.alpha.comp1 != b.alpha.comp1 || a.alpha.ref1 != b.alpha.ref1) return false;
  if (a.blend.mode != b.blend.mode || a.blend.src != b.blend.src || a.blend.dst != b.blend.dst || a.blend.logic != b.blend.logic) return false;
  if (a.z.compare != b.z.compare || a.z.func != b.z.func || a.z.update != b.z.update) return false;
  if (a.fog.type != b.fog.type || !fbits(a.fog.startZ, b.fog.startZ) || !fbits(a.fog.endZ, b.fog.endZ) || !fbits(a.fog.nearZ, b.fog.nearZ) || !fbits(a.fog.farZ, b.fog.farZ) ||
      a.fog.r != b.fog.r || a.fog.g != b.fog.g || a.fog.b != b.fog.b) return false;
  return fbits(a.projFar, b.projFar) && a.texmapScaled == b.texmapScaled;
}
bool eq(const Options& a, const Options& b) {
  return std::memcmp(&a.approxLimit, &b.approxLimit, sizeof(double)) == 0 && a.verifySamples == b.verifySamples && a.seed == b.seed && fbits(a.frameFogRange, b.frameFogRange) && a.flipTexT == b.flipTexT && a.multipass == b.multipass && a.approximate == b.approximate && a.approxEffort == b.approxEffort;
}

}  // namespace

uint64_t hashDrawKey(const GxDrawState& g, const Options& o) {
  Hasher H;
  feed(H, g);
  feed(H, o);
  return (uint64_t(H.a) << 32 | H.b) * 0x9E3779B97F4A7C15ULL;
}

bool sameDrawKey(const GxDrawState& a, const Options& oa, const GxDrawState& b, const Options& ob) { return eq(a, b) && eq(oa, ob); }

bool sameDrawKeyIgnoringPoseCount(const GxDrawState& a, const GxDrawState& b) { return eq(a,b,true); }

// (Claude v121) draw replay identity: a 128-bit hash of everything sameDrawKey compares (pose counts included), computed where the draw was
// recorded (FIFO worker) so the render core compares two words instead of walking the state's vectors
void replayDrawKey(const GxDrawState& g, uint64_t out[2]) {
  Hasher H; H.h = 0x243F6A8885A308D3ull; H.strong = true;
  feed(H, g);
  out[0] = (uint64_t(H.a) << 32 | H.b); out[1] = H.h;
}

// (Claude v105 diag) on a compile miss: which key parts differ from the closest cached entry (fewest differing parts)
void (*g_drawCacheMissLog)(const char*) = nullptr;
static void describeMiss(const std::list<std::pair<const GxDrawState*, const Options*>>& all, const GxDrawState& g, const Options& o) {
  static const char* names[] = {"tev", "ind/blk", "vtx", "npos", "alpha", "blend", "z", "fogtype", "fogrange", "fogcolor", "projfar", "texscaled", "opt"};
  int best = 99; uint32_t bestMask = 0;
  for (auto& e : all) {
    const GxDrawState& a = *e.first; uint32_t m = 0;
    bool tevSame = a.tev.stages.size() == g.tev.stages.size();
    for (size_t i = 0; tevSame && i < a.tev.stages.size(); i++) tevSame = eq(a.tev.stages[i], g.tev.stages[i]);
    if (!tevSame) m |= 1;
    if (a.indirect != g.indirect || a.importBlockers != g.importBlockers) m |= 2;
    if (!eq(a.vtx, g.vtx)) m |= 4;
    if (a.numPosMtx != g.numPosMtx) m |= 8;
    if (a.alpha.comp0 != g.alpha.comp0 || a.alpha.ref0 != g.alpha.ref0 || a.alpha.op != g.alpha.op || a.alpha.comp1 != g.alpha.comp1 || a.alpha.ref1 != g.alpha.ref1) m |= 16;
    if (a.blend.mode != g.blend.mode || a.blend.src != g.blend.src || a.blend.dst != g.blend.dst || a.blend.logic != g.blend.logic) m |= 32;
    if (a.z.compare != g.z.compare || a.z.func != g.z.func || a.z.update != g.z.update) m |= 64;
    if (a.fog.type != g.fog.type) m |= 128;
    if (!fbits(a.fog.startZ, g.fog.startZ) || !fbits(a.fog.endZ, g.fog.endZ) || !fbits(a.fog.nearZ, g.fog.nearZ) || !fbits(a.fog.farZ, g.fog.farZ)) m |= 256;
    if (a.fog.r != g.fog.r || a.fog.g != g.fog.g || a.fog.b != g.fog.b) m |= 512;
    if (!fbits(a.projFar, g.projFar)) m |= 1024;
    if (a.texmapScaled != g.texmapScaled) m |= 2048;
    if (!eq(*e.second, o)) m |= 4096;
    const int c = __builtin_popcount(m);
    if (c < best) { best = c; bestMask = m; }
  }
  char buf[256]; int n = snprintf(buf, sizeof buf, "[dc-miss] stages %zu fogtype %d projFar %g fog %g..%g diff:", g.tev.stages.size(), int(g.fog.type), double(g.projFar), double(g.fog.startZ), double(g.fog.endZ));
  for (int i = 0; i < 13; i++) if (bestMask & (1u << i)) n += snprintf(buf + n, sizeof buf - n, " %s", names[i]);
  if (best == 99) snprintf(buf + n, sizeof buf - n, " (empty cache)");
  g_drawCacheMissLog(buf);
}

std::shared_ptr<const PicaDraw> DrawCompileCache::get(const GxDrawState& g, const Options& opt) {
  st_.lookups++;
  // Consecutive batches often share a material. Exact field-wise comparison
  // avoids hashing the whole key; the existing MRU needs no list splice.
  if (recentKeyFastPath_ && !lru_.empty()) {
    const Entry& recent = lru_.front();
    if (sameDrawKey(recent.key, recent.opt, g, opt)) {
      st_.hits++;
      return recent.value;
    }
  }
  const uint64_t h = hashDrawKey(g, opt) & mask_;
  auto range = byHash_.equal_range(h);
  for (auto it = range.first; it != range.second; ++it) {
    Entry& e = *it->second;
    if (sameDrawKey(e.key, e.opt, g, opt)) {
      lru_.splice(lru_.begin(), lru_, it->second);   // iterators stay valid
      st_.hits++;
      return e.value;
    }
  }
  st_.misses++;
  if (g_drawCacheMissLog) { std::list<std::pair<const GxDrawState*, const Options*>> all; for (auto& x : lru_) all.push_back({&x.key, &x.opt}); describeMiss(all, g, opt); }
  Entry e;
  e.hash = h; e.key = g; e.opt = opt;
  extern uint64_t (*g_compileClock)(); extern uint64_t g_compilePhase[8];
  const uint64_t cc0 = g_compileClock ? g_compileClock() : 0;
  e.value = std::make_shared<PicaDraw>(compileDraw(g, opt));
  if (g_compileClock) g_compilePhase[5] += g_compileClock() - cc0;
  lru_.push_front(std::move(e));
  byHash_.emplace(h, lru_.begin());
  std::shared_ptr<const PicaDraw> out = lru_.front().value;
  while (lru_.size() > max_) {
    const Entry& last = lru_.back();
    auto r = byHash_.equal_range(last.hash);
    for (auto it = r.first; it != r.second; ++it) if (it->second == std::prev(lru_.end())) { byHash_.erase(it); break; }
    lru_.pop_back();
    st_.evictions++;
  }
  return out;
}

void DrawCompileCache::clear() {
  lru_.clear();
  byHash_.clear();
}

}  // namespace t2p
