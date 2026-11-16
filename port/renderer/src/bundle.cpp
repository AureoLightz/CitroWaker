#include "bundle.h"

#include <algorithm>
#include <cstring>

// The fingerprint comes from the build: -DT2P_SRC_FINGERPRINT=0x...ull (tools/fingerprint.sh), or a generated src/fingerprint_gen.h (tools/hw6/package.sh writes one into the sources it
// ships, because the device build line is not ours to change). A build with neither has fingerprint 0 and only accepts bundles made the same way.
#if !defined(T2P_SRC_FINGERPRINT) && defined(__has_include)
#if __has_include("fingerprint_gen.h")
#include "fingerprint_gen.h"
#endif
#endif
#ifndef T2P_SRC_FINGERPRINT
#define T2P_SRC_FINGERPRINT 0ull
#endif

namespace t2p {

// ---------------------------------------------------------------- byte I/O
void ByteWriter::f32(float v) { uint32_t b; std::memcpy(&b, &v, 4); u32(b); }
void ByteWriter::f64(double v) { uint64_t b; std::memcpy(&b, &v, 8); u64(b); }
float ByteReader::f32() { const uint32_t b = u32(); float v; std::memcpy(&v, &b, 4); return v; }
double ByteReader::f64() { const uint64_t b = u64(); double v; std::memcpy(&v, &b, 8); return v; }

std::string ByteReader::str(size_t maxLen) {
  const uint32_t n = u32();
  if (!ok_ || n > maxLen || n > remaining()) { fail(); return std::string(); }
  std::string s(reinterpret_cast<const char*>(p_ + at_), n);
  at_ += n;
  return s;
}

uint32_t ByteReader::count(uint32_t max, size_t minBytes) {
  const uint32_t n = u32();
  if (!ok_ || n > max || (minBytes && size_t(n) > remaining() / minBytes)) { fail(); return 0; }
  return n;
}

// ---------------------------------------------------------------- TEV material
namespace {

void putChan(ByteWriter& w, const TevChan& c) {
  for (Leaf l : c.in) w.u8(l);
  w.u8(c.sub); w.f32(c.bias); w.f32(c.scale); w.u8(c.clamp); w.u8(c.out); w.u8(c.cmp);
}
bool getChan(ByteReader& r, TevChan& c) {
  for (Leaf& l : c.in) { const uint8_t v = r.u8(); if (v >= L_COUNT) r.fail(); l = Leaf(v); }
  c.sub = r.u8() != 0; c.bias = r.f32(); c.scale = r.f32(); c.clamp = r.u8() != 0; c.out = r.u8(); c.cmp = r.u8();
  if (c.out > 3 || c.cmp > 2) r.fail();
  return r.ok();
}
void putKSel(ByteWriter& w, const KSel& k) { w.u8(k.kind); w.f32(k.fixed); w.u8(k.idx); w.u8(k.chan); }
bool getKSel(ByteReader& r, KSel& k) {
  const uint8_t kind = r.u8();
  if (kind > KSel::Chan) r.fail();
  k.kind = KSel::Kind(kind); k.fixed = r.f32(); k.idx = r.u8(); k.chan = r.u8();
  if (k.idx > 3 || k.chan > 3) r.fail();
  return r.ok();
}

void putArg(ByteWriter& w, const PArg& a) { w.u8(a.src); w.u8(a.opr); w.u8(a.snap); }
bool getArg(ByteReader& r, PArg& a) {
  const uint8_t s = r.u8(), o = r.u8();
  if (s > P_BUF || o > OPR_1B) r.fail();
  a.src = PSrc(s); a.opr = POpr(o); a.snap = r.u8() != 0;
  return r.ok();
}
void putPChan(ByteWriter& w, const PChan& c) { w.u8(c.op); for (const PArg& a : c.arg) putArg(w, a); w.u8(c.scale); }
bool getPChan(ByteReader& r, PChan& c) {
  const uint8_t op = r.u8();
  if (op >= OP_COUNT) r.fail();
  c.op = POp(op);
  for (PArg& a : c.arg) getArg(r, a);
  c.scale = r.u8();
  if (c.scale != 1 && c.scale != 2 && c.scale != 4) r.fail();
  return r.ok();
}

}  // namespace

void putTevMaterial(ByteWriter& w, const TevMaterial& m, bool withTexcoord) {
  w.u32(uint32_t(m.stages.size()));
  for (const TevStage& s : m.stages) {
    putChan(w, s.c); putChan(w, s.a); putKSel(w, s.kc); putKSel(w, s.ka);
    w.i32(s.texmap); w.i32(s.rasChan);
    if (withTexcoord) w.i32(s.texcoord);
    for (int i = 0; i < 4; i++) w.u8(s.texSwap[i]);
    for (int i = 0; i < 4; i++) w.u8(s.rasSwap[i]);
    for (int i = 0; i < 3; i++) w.u8(s.regBufC[i]);
    for (int i = 0; i < 3; i++) w.u8(s.regBufA[i]);
  }
}

bool getTevMaterial(ByteReader& r, TevMaterial& m) {
  m.stages.clear();
  const uint32_t n = r.count(64, 40);
  for (uint32_t i = 0; i < n && r.ok(); i++) {
    TevStage s;
    getChan(r, s.c); getChan(r, s.a); getKSel(r, s.kc); getKSel(r, s.ka);
    s.texmap = r.i32(); s.rasChan = r.i32(); s.texcoord = r.i32();
    for (int k = 0; k < 4; k++) { s.texSwap[k] = r.u8(); if (s.texSwap[k] > 3) r.fail(); }
    for (int k = 0; k < 4; k++) { s.rasSwap[k] = r.u8(); if (s.rasSwap[k] > 3) r.fail(); }
    for (int k = 0; k < 3; k++) s.regBufC[k] = r.u8() != 0;
    for (int k = 0; k < 3; k++) s.regBufA[k] = r.u8() != 0;
    m.stages.push_back(s);
  }
  if (!r.ok()) m.stages.clear();
  return r.ok();
}

// ---------------------------------------------------------------- translation
void putTranslation(ByteWriter& w, const Translation& t) {
  w.u8(uint32_t(int(t.overall)));
  w.u32(uint32_t(t.prog.stages.size()));
  for (const PStage& s : t.prog.stages) {
    putPChan(w, s.c); putPChan(w, s.a);
    for (const KComp& k : s.k) { w.u8(k.set); w.u8(k.sym); w.u8(k.chan); w.u8(k.inv); w.f32(k.fixed); }
    w.u8(s.updBufC); w.u8(s.updBufA);
  }
  w.u32(uint32_t(t.notes.size()));
  for (const StageNote& n : t.notes) {
    w.i32(n.gcStage); w.u8(uint32_t(int(n.color))); w.u8(uint32_t(int(n.alpha))); w.i32(n.picaStages); w.f64(n.colorErr255); w.f64(n.alphaErr255); w.str(n.text);
  }
  w.f64(t.verify.logicMaxErr255); w.f64(t.verify.maxErr255); w.f64(t.verify.meanErr255); w.i32(t.verify.samples);
}

bool getTranslation(ByteReader& r, Translation& t) {
  t = Translation();
  auto fid = [&](uint8_t v) { if (v > 2) r.fail(); return Fidelity(v); };
  t.overall = fid(r.u8());
  const uint32_t ns = r.count(256, 50);
  for (uint32_t i = 0; i < ns && r.ok(); i++) {
    PStage s;
    getPChan(r, s.c); getPChan(r, s.a);
    for (KComp& k : s.k) {
      k.set = r.u8() != 0; k.sym = r.u8(); k.chan = r.u8(); k.inv = r.u8() != 0; k.fixed = r.f32();
      if (k.sym > SYM_R0 + 2 || k.chan > 3) r.fail();
    }
    s.updBufC = r.u8() != 0; s.updBufA = r.u8() != 0;
    t.prog.stages.push_back(s);
  }
  const uint32_t nn = r.count(1 << 16, 30);
  for (uint32_t i = 0; i < nn && r.ok(); i++) {
    StageNote n;
    n.gcStage = r.i32(); n.color = fid(r.u8()); n.alpha = fid(r.u8()); n.picaStages = r.i32(); n.colorErr255 = r.f64(); n.alphaErr255 = r.f64(); n.text = r.str();
    t.notes.push_back(std::move(n));
  }
  t.verify.logicMaxErr255 = r.f64(); t.verify.maxErr255 = r.f64(); t.verify.meanErr255 = r.f64(); t.verify.samples = r.i32();
  if (!r.ok()) t = Translation();
  return r.ok();
}

// ---------------------------------------------------------------- keys
std::string translateKey(const TevMaterial& m, const Options& o) {
  ByteWriter w;
  w.u8('T');
  putTevMaterial(w, m, false);
  w.f64(o.approxLimit);
  w.u64(o.seed);
  return std::string(w.buf.begin(), w.buf.end());
}

std::string approxKey(const TevMaterial& m) {
  ByteWriter w;
  w.u8('A');
  putTevMaterial(w, m, true);
  return std::string(w.buf.begin(), w.buf.end());
}

// ---------------------------------------------------------------- the bundle
const Translation* MaterialBundle::findTranslation(const std::string& key) const {
  const auto it = tr_.find(key);
  return it == tr_.end() ? nullptr : &it->second;
}
const ApproxEntry* MaterialBundle::findApprox(const std::string& key) const {
  const auto it = ap_.find(key);
  return it == ap_.end() ? nullptr : &it->second;
}
void MaterialBundle::addTranslation(const std::string& key, const Translation& t) { tr_.emplace(key, t); }
void MaterialBundle::addApprox(const std::string& key, const ApproxEntry& e) { ap_.emplace(key, e); }

namespace {
constexpr uint32_t kMagic = 0x42503254u;   // "T2PB"
constexpr uint32_t kVersion = 1;
constexpr size_t kHeader = 4 + 4 + 8 + 4 + 4;

uint64_t fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
  return h;
}
}  // namespace

std::vector<uint8_t> MaterialBundle::serialize(uint64_t fingerprint) const {
  std::vector<const std::string*> tk, ak;
  for (const auto& kv : tr_) tk.push_back(&kv.first);
  for (const auto& kv : ap_) ak.push_back(&kv.first);
  auto less = [](const std::string* a, const std::string* b) { return *a < *b; };
  std::sort(tk.begin(), tk.end(), less);
  std::sort(ak.begin(), ak.end(), less);
  ByteWriter w;
  w.u32(kMagic); w.u32(kVersion); w.u64(fingerprint); w.u32(uint32_t(tk.size())); w.u32(uint32_t(ak.size()));
  for (const std::string* k : tk) { w.str(*k); putTranslation(w, tr_.at(*k)); }
  for (const std::string* k : ak) {
    const ApproxEntry& e = ap_.at(*k);
    w.str(*k); putTevMaterial(w, e.material, true); w.u8(e.relaxed); w.str(e.note);
  }
  w.u64(fnv1a(w.buf.data(), w.buf.size()));
  return w.buf;
}

bool MaterialBundle::load(const uint8_t* data, size_t size, uint64_t fingerprint, std::string* err) {
  auto bad = [&](const char* why) { if (err) *err = why; return false; };
  if (!data || size < kHeader + 8) return bad("too short to be a material bundle");
  ByteReader h(data, kHeader);
  if (h.u32() != kMagic) return bad("not a material bundle");
  if (h.u32() != kVersion) return bad("unknown bundle version");
  const uint64_t fp = h.u64();
  const uint32_t nt = h.u32(), na = h.u32();
  ByteReader tail(data + size - 8, 8);
  if (fnv1a(data, size - 8) != tail.u64()) return bad("checksum mismatch (damaged or truncated file)");
  if (fp != fingerprint) return bad("built from other translator sources (rebuild the bundle)");
  ByteReader r(data + kHeader, size - kHeader - 8);
  if (nt > r.remaining() / 8 || na > r.remaining() / 8) return bad("entry counts do not fit the file");
  std::vector<std::pair<std::string, Translation>> ts;
  std::vector<std::pair<std::string, ApproxEntry>> as;
  ts.reserve(nt); as.reserve(na);
  for (uint32_t i = 0; i < nt; i++) {
    std::pair<std::string, Translation> e;
    e.first = r.str(1 << 16);
    if (!getTranslation(r, e.second)) return bad("damaged translation entry");
    ts.push_back(std::move(e));
  }
  for (uint32_t i = 0; i < na; i++) {
    std::pair<std::string, ApproxEntry> e;
    e.first = r.str(1 << 16);
    if (!getTevMaterial(r, e.second.material)) return bad("damaged approximation entry");
    e.second.relaxed = r.u8() != 0;
    e.second.note = r.str();
    if (!r.ok()) return bad("damaged approximation entry");
    as.push_back(std::move(e));
  }
  if (!r.ok() || r.remaining() != 0) return bad("trailing or missing bytes");
  for (auto& e : ts) tr_.emplace(std::move(e.first), std::move(e.second));
  for (auto& e : as) ap_.emplace(std::move(e.first), std::move(e.second));
  return true;
}

// ---------------------------------------------------------------- hooks
namespace {
const MaterialBundle* g_lookup = nullptr;
MaterialBundle* g_recorder = nullptr;
BundleStats g_stats;
}  // namespace

void setBundleLookup(const MaterialBundle* b) { g_lookup = b; }
void setBundleRecorder(MaterialBundle* b) { g_recorder = b; }
const MaterialBundle* bundleLookup() { return g_lookup; }
MaterialBundle* bundleRecorder() { return g_recorder; }
BundleStats bundleStats() { return g_stats; }
void resetBundleStats() { g_stats = BundleStats(); }
void bundleCountHit() { g_stats.hits++; }
void bundleCountMiss(double seconds) { g_stats.misses++; g_stats.missSeconds += seconds; }
void bundleCountApproxHit() { g_stats.approxHits++; }
uint64_t sourceFingerprint() { return T2P_SRC_FINGERPRINT; }

}  // namespace t2p
