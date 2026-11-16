#include "gxretain.h"

#include <algorithm>
#include <cstring>

namespace t2p {

// A registered array's memory and the snapshot of the bytes retained draws were decoded from.
struct GxRetainer::Watch {
  const uint8_t* base = nullptr;
  uint32_t lo = 0, hi = 0;                 // the snapshot holds base[lo, hi)
  std::vector<uint8_t> snap;
  uint32_t version = 0;                    // bumped whenever the snapshot is retaken over bytes that changed
  uint64_t vChunk = ~0ull;                 // during process() call vChunk, memory equals the snapshot over [vLo, vHi)
  uint32_t vLo = 0, vHi = 0;
  uint64_t lastFrame = 0;
};

GxRetainer::GxRetainer() = default;
GxRetainer::~GxRetainer() = default;

namespace {
inline uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0xFF51AFD7ED558CCDull; }
size_t capBytes(const GxDraw& d) {
  const GxVertices& W = d.v;
  size_t n = W.pos.capacity() * 4 + W.nrm.capacity() * 4 + W.pmIdx.capacity() + d.tris.capacity() * 4;
  for (int c = 0; c < 2; c++) n += W.clr[c].capacity();
  for (int t = 0; t < 8; t++) n += W.tex[t].capacity() * 4 + W.texMtxIdx[t].capacity();
  return n + sizeof(RetainedGeometry);
}
// memcmp != 0, word at a time whatever the alignment (newlib's memcmp falls back to a byte loop unless both pointers are word aligned; FIFO data is not).
// ARMv6+ / x86 load unaligned words directly (memcpy of 4 bytes compiles to one load).
inline bool differ(const uint8_t* a, const uint8_t* b, size_t n) {
  while (n >= 16) {
    uint32_t x[4], y[4];
    std::memcpy(x, a, 16); std::memcpy(y, b, 16);
    if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) return true;
    a += 16; b += 16; n -= 16;
  }
  while (n >= 4) { uint32_t x, y; std::memcpy(&x, a, 4); std::memcpy(&y, b, 4); if (x != y) return true; a += 4; b += 4; n -= 4; }
  while (n--) if (*a++ != *b++) return true;
  return false;
}
template <class V> void prefix(V& dst, const V& src, size_t n) { dst.assign(src.begin(), src.begin() + std::ptrdiff_t(std::min(n, src.size()))); }
inline bool indexed(const GxAttrPlan& a) { return a.kind != GxAttrPlan::Byte && (a.type == GXA_INDEX8 || a.type == GXA_INDEX16); }
}  // namespace

bool GxRetainer::retainable(const GxDrawInputs& in) {
  if (!in.fast || in.vertices == 0) return false;
  for (int k = 0; k < in.nAttrs; k++) {
    const GxAttrPlan& a = in.attrs[k];
    if (!indexed(a)) continue;
    if (!a.base || a.streamBytes != (a.type == GXA_INDEX16 ? 2u : 1u)) return false;   // guest-memory arrays, NBT triple indices: always decoded
  }
  return true;
}

uint64_t GxRetainer::keyOf(const GxDrawInputs& in) {
  if (in.shapeKey) return mix(mix(in.shapeKey, in.shapeDraw), uint64_t(in.cmd) << 32 | in.vertices);   // (Claude v95) no stream bytes read
  uint64_t h = mix(in.cmd, in.vertices);
  h = mix(h, in.vsize | uint64_t(uint32_t(in.partCount)) << 32);
  for (int i = 0; i < 5; i++) h = mix(h, in.fmtRaw[i]);
  const size_t n = std::min<size_t>(32, size_t(in.vertices) * in.vsize);
  for (size_t i = 0; i < n; i++) h = mix(h, in.stream[i]);
  return h;
}

bool GxRetainer::verify(const Binding& bd, uint32_t c0, uint32_t c1, uint64_t chunk) {
  Watch* w = bd.w;
  if (w->version != bd.version) return false;
  if (c0 < w->lo || c1 > w->hi) return false;          // (cannot happen: a snapshot only grows while its version stays)
  w->lastFrame = frame_;
  const uint8_t* mem = w->base;
  const uint8_t* snap = w->snap.data() - w->lo;        // snap[x] for x in [lo, hi)
  if (w->vChunk != chunk) {
    st_.verifyBytes += c1 - c0;
    if (differ(mem + c0, snap + c0, c1 - c0)) return false;
    w->vChunk = chunk; w->vLo = c0; w->vHi = c1;
    return true;
  }
  if (c0 >= w->vLo && c1 <= w->vHi) return true;
  // the draw's own bytes outside the verified range
  const uint32_t a0 = std::max(c0, w->vLo), a1 = std::min(c1, w->vHi);   // overlap (empty when a0 >= a1)
  if (a0 < a1) {
    if (c0 < a0) { st_.verifyBytes += a0 - c0; if (differ(mem + c0, snap + c0, a0 - c0)) return false; }
    if (a1 < c1) { st_.verifyBytes += c1 - a1; if (differ(mem + a1, snap + a1, c1 - a1)) return false; }
    w->vLo = std::min(w->vLo, c0); w->vHi = std::max(w->vHi, c1);
    return true;
  }
  st_.verifyBytes += c1 - c0;
  if (differ(mem + c0, snap + c0, c1 - c0)) return false;
  // disjoint from the verified range: verify the gap too and keep the hull (one interval per array and call; ~0.2 MB per frame on the fixture)
  const uint32_t g0 = c1 <= w->vLo ? c1 : w->vHi, g1 = c1 <= w->vLo ? w->vLo : c0;
  st_.verifyBytes += g1 - g0;
  if (differ(mem + g0, snap + g0, g1 - g0) == 0) { w->vLo = std::min(w->vLo, c0); w->vHi = std::max(w->vHi, c1); }
  else if (c1 - c0 > w->vHi - w->vLo) { w->vLo = c0; w->vHi = c1; }
  return true;
}

GxRetainer::Watch* GxRetainer::snapshot(const uint8_t* base, uint32_t b0, uint32_t b1, uint64_t chunk, uint32_t& version) {
  std::unique_ptr<Watch>& slot = watches_[base];
  if (!slot) { slot.reset(new Watch); slot->base = base; }
  Watch* w = slot.get();
  w->lastFrame = frame_;
  if (w->vChunk == chunk && b0 >= w->vLo && b1 <= w->vHi && b0 >= w->lo && b1 <= w->hi) { version = w->version; return w; }
  if (w->snap.empty()) {
    w->lo = b0; w->hi = b1;
    w->snap.assign(base + b0, base + b1);
    st_.snapshotBytes += b1 - b0;
  } else {
    // Bytes the snapshot holds that differ from memory now: every record made against them is invalid (new version). Then the snapshot is retaken over the
    // hull of its old range and [b0, b1) (gap bytes are taken from memory now; no record reads them).
    const bool verified = w->vChunk == chunk && w->lo >= w->vLo && w->hi <= w->vHi;
    if (!verified) { st_.verifyBytes += w->hi - w->lo; if (differ(base + w->lo, w->snap.data(), w->hi - w->lo)) w->version++; }
    const uint32_t lo = std::min(w->lo, b0), hi = std::max(w->hi, b1);
    if (lo != w->lo || hi != w->hi || !verified) {
      w->snap.assign(base + lo, base + hi);
      st_.snapshotBytes += hi - lo;
      w->lo = lo; w->hi = hi;
    }
  }
  w->vChunk = chunk; w->vLo = w->lo; w->vHi = w->hi;
  version = w->version;
  return w;
}

bool GxRetainer::match(const Batch& b, const DrawRec& r, const GxDrawInputs& in, uint64_t chunk) {
  lastFail_ = 2;
  if (r.cmd != in.cmd || r.vertices != in.vertices || r.vsize != in.vsize || std::memcmp(r.fmt, in.fmtRaw, sizeof r.fmt)) return false;
  if (r.nParts != uint32_t(in.partCount)) return false;
  for (uint32_t p = 0; p < r.nParts; p++) {
    const uint8_t prim = in.partPrim[p * in.partStride];
    uint32_t count; std::memcpy(&count, reinterpret_cast<const uint8_t*>(in.partCount32) + p * in.partStride, 4);
    if (b.parts[r.partOff + p] != (uint32_t(prim) << 24 | count)) return false;
  }
  // (Claude v95) shape references: the same immutable list (key = address, size, model generation, format, array bindings) at the same draw
  // position decodes to the same vertices; the stream and array bytes are not compared (nor were they snapshotted)
  lastFail_ = 6;
  if (r.shapeKey || in.shapeKey) { if (r.shapeKey == in.shapeKey && r.shapeDraw == in.shapeDraw) { st_.shapeMatches++; return true; } return false; }
  lastFail_ = 3;
  const Range* rg = b.ranges.data() + r.rangeOff;
  uint32_t n = 0;
  for (int k = 0; k < in.nAttrs; k++) {
    const GxAttrPlan& a = in.attrs[k];
    if (!indexed(a)) continue;
    if (n >= r.nRange) return false;
    const Binding& bd = b.bindings[rg[n++].binding];
    if (bd.base != a.base || bd.stride != a.stride || bd.size != a.size || bd.be != a.be) return false;
  }
  if (n != r.nRange) return false;
  const size_t bytes = size_t(r.vertices) * r.vsize;
  st_.streamBytes += bytes;
  lastFail_ = 4;
  if (differ(b.stream.data() + r.streamOff, in.stream, bytes)) return false;
  lastFail_ = 5;
  for (uint32_t i = 0; i < r.nRange; i++) if (!verify(b.bindings[rg[i].binding], rg[i].b0, rg[i].b1, chunk)) return false;
  return true;
}

void GxRetainer::record(const GxDrawInputs& in, const GxDraw& D, uint32_t firstVertex, uint64_t chunk) {
  Batch& b = *building_;
  DrawRec r;
  r.cmd = in.cmd; r.vertices = in.vertices; r.vsize = in.vsize;
  std::memcpy(r.fmt, in.fmtRaw, sizeof r.fmt);
  r.partOff = uint32_t(b.parts.size()); r.nParts = uint32_t(in.partCount);
  for (int p = 0; p < in.partCount; p++) {
    uint32_t count; std::memcpy(&count, reinterpret_cast<const uint8_t*>(in.partCount32) + p * in.partStride, 4);
    b.parts.push_back(uint32_t(in.partPrim[p * in.partStride]) << 24 | count);
  }
  if (in.shapeKey) {   // (Claude v95) nothing of the stream or the arrays is kept: see match()
    r.shapeKey = in.shapeKey; r.shapeDraw = in.shapeDraw; r.streamOff = uint32_t(b.stream.size());
    int m = -1;
    if (D.v.hasPmIdx) for (size_t i = firstVertex; i < D.v.pmIdx.size(); i++) m = std::max<int>(m, D.v.pmIdx[i] / 3);
    r.maxPm = m;
    r.vertsEnd = D.v.count; r.trisEnd = uint32_t(D.tris.size());
    b.recs.push_back(r);
    return;
  }
  const size_t bytes = size_t(in.vertices) * in.vsize;
  r.streamOff = uint32_t(b.stream.size());
  b.stream.insert(b.stream.end(), in.stream, in.stream + bytes);
  for (int k = 0; k < in.nAttrs; k++) {
    const GxAttrPlan& a = in.attrs[k];
    if (!indexed(a)) continue;
    uint32_t lo = ~0u, hi = 0;
    const uint8_t* s = in.stream + a.offset;
    if (a.type == GXA_INDEX16) for (uint32_t v = 0; v < in.vertices; v++, s += in.vsize) { const uint32_t i = uint32_t(s[0]) << 8 | s[1]; lo = std::min(lo, i); hi = std::max(hi, i); }
    else for (uint32_t v = 0; v < in.vertices; v++, s += in.vsize) { lo = std::min<uint32_t>(lo, *s); hi = std::max<uint32_t>(hi, *s); }
    Range rg;
    rg.b0 = lo * a.stride; rg.b1 = hi * a.stride + a.need;   // the bytes decoding read: elements lo..hi, `need` bytes each
    uint32_t version = 0;
    Watch* w = snapshot(a.base, rg.b0, rg.b1, chunk, version);
    uint32_t id = 0;
    while (id < b.bindings.size()) {
      const Binding& o = b.bindings[id];
      if (o.base == a.base && o.stride == a.stride && o.size == a.size && o.be == a.be && o.w == w && o.version == version) break;
      id++;
    }
    if (id == b.bindings.size()) b.bindings.push_back(Binding{a.base, a.stride, a.size, a.be, w, version});
    rg.binding = id;
    if (r.nRange == 0) r.rangeOff = uint32_t(b.ranges.size());
    b.ranges.push_back(rg);
    r.nRange++;
  }
  int m = -1;
  if (D.v.hasPmIdx) for (size_t i = firstVertex; i < D.v.pmIdx.size(); i++) m = std::max<int>(m, D.v.pmIdx[i] / 3);
  r.maxPm = m;
  r.vertsEnd = D.v.count; r.trisEnd = uint32_t(D.tris.size());
  b.recs.push_back(r);
}

void GxRetainer::materialize(GxDraw& D) {
  const GxDraw& S = cand_->geo->d;
  const DrawRec& last = cand_->recs[cursor_ - 1];
  const size_t nv = last.vertsEnd, nt = last.trisEnd;
  GxVertices& V = D.v;
  V.hasPos = S.v.hasPos; V.hasNrm = S.v.hasNrm; V.hasPmIdx = S.v.hasPmIdx;
  for (int c = 0; c < 2; c++) V.hasClr[c] = S.v.hasClr[c];
  for (int t = 0; t < 8; t++) { V.hasTex[t] = S.v.hasTex[t]; V.hasTexMtxIdx[t] = S.v.hasTexMtxIdx[t]; }
  prefix(V.pos, S.v.pos, nv * 3); prefix(V.nrm, S.v.nrm, nv * 3); prefix(V.pmIdx, S.v.pmIdx, nv);
  for (int c = 0; c < 2; c++) prefix(V.clr[c], S.v.clr[c], nv * 4);
  for (int t = 0; t < 8; t++) { prefix(V.tex[t], S.v.tex[t], nv * 2); prefix(V.texMtxIdx[t], S.v.texMtxIdx[t], nv); }
  prefix(D.tris, S.tris, nt);
  V.count = uint32_t(nv);
  D.retained = false; D.retainedTris = 0;
  // the prefix's records start the batch that will be published (its geometry is now D's)
  auto nb = std::make_shared<Batch>();
  nb->key = cand_->key;
  nb->bindings = cand_->bindings;
  for (uint32_t i = 0; i < cursor_; i++) {
    DrawRec r = cand_->recs[i];
    const uint32_t* pp = cand_->parts.data() + r.partOff;
    r.partOff = uint32_t(nb->parts.size()); nb->parts.insert(nb->parts.end(), pp, pp + r.nParts);
    const uint8_t* sp = cand_->stream.data() + r.streamOff;
    r.streamOff = uint32_t(nb->stream.size()); if (!r.shapeKey) nb->stream.insert(nb->stream.end(), sp, sp + size_t(r.vertices) * r.vsize);
    const Range* rp = cand_->ranges.data() + r.rangeOff;
    r.rangeOff = uint32_t(nb->ranges.size()); nb->ranges.insert(nb->ranges.end(), rp, rp + r.nRange);
    nb->recs.push_back(r);
  }
  building_ = std::move(nb);
  cand_.reset();
  st_.materialized++;
}

bool GxRetainer::offer(const GxDrawInputs& in, GxDraw& D, bool newBatch, uint64_t chunk, int& drawMaxPm) {
  st_.draws++;
  if (in.shapeKey) st_.shapeOffered++;
  if (newBatch) {
    open_ = true; cursor_ = 0; cand_.reset(); building_.reset(); mode_ = Off;
    reason_ = 0;
    if (!retainable(in)) { st_.notRetainable++; return false; }
    const uint64_t key = keyOf(in);
    auto it = prev_.find(key);
    reason_ = 1;
    if (it != prev_.end())
      for (const std::shared_ptr<Batch>& c : it->second)
        if (match(*c, c->recs[0], in, chunk)) { cand_ = c; break; }
    if (in.shapeKey && !cand_) { if (it == prev_.end()) st_.shapeNewNoPrev++; else st_.shapeNewMiss++; }
    if (cand_) {
      mode_ = Matching; cursor_ = 1;
      D.retained = true; D.retainedTris = cand_->recs[0].trisEnd;
      drawMaxPm = cand_->recs[0].maxPm;
      st_.skipped++;
      return true;
    }
    if (it != prev_.end()) reason_ = lastFail_;
    mode_ = Fresh;
    building_ = std::make_shared<Batch>();
    building_->key = key;
    return false;
  }
  if (!open_) return false;
  if (mode_ == Matching) {
    if (cursor_ < cand_->recs.size() && retainable(in) && match(*cand_, cand_->recs[cursor_], in, chunk)) {
      const DrawRec& r = cand_->recs[cursor_++];
      D.retainedTris = r.trisEnd;
      drawMaxPm = r.maxPm;
      st_.skipped++;
      return true;
    }
    if (in.shapeKey) st_.shapeMidMiss++;
    reason_ = 7;
    materialize(D);
    mode_ = Fresh;
  }
  if (mode_ == Fresh && !retainable(in)) { mode_ = Off; building_.reset(); st_.notRetainable++; reason_ = 0; }
  return false;
}

void GxRetainer::decoded(const GxDrawInputs& in, const GxDraw& D, uint32_t firstVertex, size_t, bool arrayError, uint64_t chunk) {
  if (!open_ || mode_ != Fresh) return;
  if (arrayError) { mode_ = Off; building_.reset(); return; }
  record(in, D, firstVertex, chunk);
}

void GxRetainer::use(const std::shared_ptr<Batch>& b) {
  if (b->usedFrame == frame_) return;
  b->usedFrame = frame_;
  cur_.push_back(b);
  heldNext_ += b->bytes;
}

std::shared_ptr<const RetainedGeometry> GxRetainer::closeBatch(GxDraw& D, int maxPm) {
  if (!open_) return nullptr;
  open_ = false;
  st_.batches++;
  if (mode_ == Matching) {
    if (cursor_ == cand_->recs.size()) {
      use(cand_);
      std::shared_ptr<const RetainedGeometry> g = cand_->geo;
      cand_.reset(); mode_ = None;
      st_.retainedBatches++;
      return g;
    }
    materialize(D);
    mode_ = Fresh;
    reason_ = 7;
  }
  if (mode_ == Fresh && building_ && !building_->recs.empty() && heldNext_ > budget_) reason_ = 8;
  st_.missBatches[reason_]++; st_.missVerts[reason_] += D.v.count;
  std::shared_ptr<const RetainedGeometry> out;
  if (mode_ == Fresh && building_ && !building_->recs.empty() && heldNext_ <= budget_) {
    auto g = std::make_shared<RetainedGeometry>();
    g->d = std::move(D);
    D.v.count = g->d.v.count;                    // D keeps its header; its geometry is now the shared one
    D.retained = true; D.retainedTris = uint32_t(g->d.tris.size());
    g->id = ++nextId_;
    g->maxPm = maxPm;
    g->bytes = capBytes(g->d);
    Batch& b = *building_;
    b.geo = g;
    b.bytes = g->bytes + b.recs.capacity() * sizeof(DrawRec) + b.stream.capacity() + b.parts.capacity() * 4 + b.bindings.capacity() * sizeof(Binding) +
              b.ranges.capacity() * sizeof(Range) + sizeof(Batch);
    use(building_);
    st_.published++;
    out = std::move(g);
  }
  else if (mode_ == Fresh && building_ && !building_->recs.empty()) st_.overBudget++;
  else st_.closedOff++;
  building_.reset(); cand_.reset(); mode_ = None;
  return out;
}

void GxRetainer::endFrame() {
  prev_.clear();
  for (const std::shared_ptr<Batch>& b : cur_) prev_[b->key].push_back(b);
  cur_.clear();
  held_ = heldNext_;
  heldNext_ = 0;
  frame_++;
  size_t snaps = 0;
  for (auto it = watches_.begin(); it != watches_.end();) {
    if (it->second->lastFrame + 2 < frame_) it = watches_.erase(it);
    else { snaps += it->second->snap.capacity(); ++it; }
  }
  held_ += snaps;
}

void GxRetainer::clear() {
  prev_.clear(); cur_.clear(); watches_.clear();
  open_ = false; mode_ = None; cand_.reset(); building_.reset(); cursor_ = 0;
  held_ = heldNext_ = 0;
}

}  // namespace t2p
