#include <mutex>
#include <set>
#include "c3dplan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <list>
#include <string_view>
#include <unordered_map>

namespace t2p {
const std::string* internUniformName(const std::string& name) {   // (Claude v122)
  static std::mutex m; static std::set<std::string> names;
  std::lock_guard<std::mutex> g(m);
  return &*names.insert(name).first;
}

namespace {

std::string hex64(uint64_t v) {
  static const char* d = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; i--, v >>= 4) s[size_t(i)] = d[v & 15];
  return s;
}

uint64_t hashStr(const std::string& s) { return hashBytes(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

template <class T> void put(std::string& s, const T& v) { s.append(reinterpret_cast<const char*>(&v), sizeof(T)); }

int popcount16(unsigned v) { int n = 0; for (; v; v &= v - 1) n++; return n; }

const HwStage kPassThrough = {0x000F000Fu, 0u, 0u, 0xFFFFFFFFu, 0u};   // C3D_TexEnvInit: REPLACE of PREVIOUS (rgb and alpha), constant white, scale 1

uint8_t wrapFor(uint8_t w) { return w == 1 ? uint8_t(ctru::REPEAT) : w == 2 ? uint8_t(ctru::MIRRORED_REPEAT) : uint8_t(ctru::CLAMP_TO_EDGE); }

C3dPlan& refuse(C3dPlan& L, const std::string& why) { L.ok = false; L.why = why; return L; }

}  // namespace

uint16_t c3dLodBias(float bias) {
  int i = int(bias * 0x100);
  if (i > 0xFFF) i = 0xFFF;
  else if (i < -0x1000) i = -0x1000;
  return uint16_t(i & 0x1FFF);
}

C3dShader* C3dShaderCache::get(const PicaDraw& d) {
  if (d.shaderMemo.cacheId == id_ && d.shaderMemo.shader) return static_cast<C3dShader*>(d.shaderMemo.shader);   // (shaders are never evicted)
  C3dShader* s = get(d.vs.source);
  d.shaderMemo.cacheId = id_; d.shaderMemo.shader = s;
  return s;
}

C3dShader* C3dShaderCache::get(const std::string& src) {
  const uint64_t key = hashStr(src);
  auto& chain = byKey_[key];
  for (auto& s : chain) if (s->source == src) return s.get();
  auto sh = std::make_unique<C3dShader>();
  sh->key = key;
  sh->source = src;
  sh->bin = assemblePica(src, "shader_" + hex64(key) + ".v.pica");
  if (!sh->bin.ok) {
    sh->error = sh->bin.error;
  } else {
    sh->inputMask = sh->bin.inputMask;
    for (const PicaBinUniform& u : sh->bin.uniforms) {
      if (u.pos < 0x20) continue;                                   // `.in` names (input registers)
      if (u.pos >= 0x80) { sh->error = "shader declares an integer / boolean uniform (" + u.name + "): not part of the host contract"; break; }
      sh->uniforms[u.name] = {c3dUniformId(u.pos), u.size, packedUniformSlot(u.name), internUniformName(u.name)};
    }
    sh->uniformsById.assign(sh->uniforms.begin(), sh->uniforms.end());
    std::sort(sh->uniformsById.begin(), sh->uniformsById.end(), [](const auto& a, const auto& b) { return a.second.id < b.second.id; });
  }
  chain.push_back(std::move(sh));
  count_++;
  return chain.back().get();
}

void c3dFinalize(C3dPlan& L) {
  // The same bytes the string-built version hashed (field by field, native layout), assembled in a stack buffer: identical hash values.
  uint8_t b[256];
  size_t n = 0;
  auto put = [&](const auto& v) { std::memcpy(b + n, &v, sizeof v); n += sizeof v; };
  for (int i = 0; i < L.attrs.count; i++) { put(L.attrs.loader[i].reg); put(L.attrs.loader[i].format); put(L.attrs.loader[i].count); }
  L.hashAttrs = hashBytes(b, n);
  n = 0;
  for (int i = 0; i < 6; i++) put(L.texenv.stage[i]);
  put(L.texenv.updateBuffer); put(L.texenv.bufferColor);
  L.hashTexEnv = hashBytes(b, n);
  n = 0;
  const C3dEffect& E = L.fx;
  put(E.zBuffer); put(E.zScale); put(E.zOffset); put(E.cull); put(E.depthTest); put(E.depthFunc); put(E.writeMask);
  put(E.alphaTest); put(E.alphaFunc); put(E.alphaRef); put(E.logicOp); put(E.logic);
  put(E.eqRgb); put(E.eqAlpha); put(E.srcRgb); put(E.dstRgb); put(E.srcAlpha); put(E.dstAlpha);
  put(E.blendColor); put(E.fog); put(E.fogColor);
  if (!E.fog) { L.hashEffect = hashBytes(b, n); return; }
  // With fog the 512-byte LUT is part of the hashed bytes. Draws of a frame share one fog range, so consecutive plans mostly hash the very same
  // bytes: remember the last input exactly (fields + LUT) and its hash (renderers are single-threaded, as the reused buffer before).
  struct Memo { bool valid = false; size_t n = 0; uint8_t fields[64]; uint32_t lut[128]; uint64_t hash = 0; };
  static Memo m;
  static_assert(sizeof(L.fogLut) == sizeof(m.lut), "fog LUT size");
  if (m.valid && m.n == n && std::memcmp(m.fields, b, n) == 0 && std::memcmp(m.lut, L.fogLut, sizeof m.lut) == 0) { L.hashEffect = m.hash; return; }
  uint8_t full[64 + sizeof(L.fogLut)];
  std::memcpy(full, b, n); std::memcpy(full + n, L.fogLut, sizeof(L.fogLut));
  L.hashEffect = hashBytes(full, n + sizeof(L.fogLut));
  if (n <= sizeof m.fields) { m.valid = true; m.n = n; std::memcpy(m.fields, b, n); std::memcpy(m.lut, L.fogLut, sizeof m.lut); m.hash = L.hashEffect; }
}

// ================================================================ plan building blocks
// (Claude Opus, 2026-11-11) The original blocks of buildPlanInto, moved unchanged into helpers so that the full build and the plan-template hit path run the
// SAME code (same checks, order, refusal strings, texture lookups). A helper that refuses sets L.ok / L.why through refuse() and returns false.
namespace {

// vertex buffer size, index range, borrowed geometry pointers (the tail of the attribute block)
bool planGeometry(C3dPlan& L, const DrawPacket& P) {
  if (P.vertexBytes() != size_t(P.stride) * P.vertexCount) { refuse(L, "vertex buffer size"); return false; }
  if (!P.indicesChecked) for (size_t k=0;k<P.indexCount();++k) { uint16_t i=P.indexData()[k]; if (i >= P.vertexCount) { refuse(L, "index " + std::to_string(i) + " beyond the " + std::to_string(P.vertexCount) + " vertices"); return false; } }
  L.vertices = P.vertexData(); L.vertexBytes = uint32_t(P.vertexBytes());
  L.indices = P.indexData(); L.indexCount = uint32_t(P.indexCount());
  return true;
}

// texenv words of the packet (before the destination-alpha stage the effect block may append)
bool planTexEnv(C3dPlan& L, const DrawPacket& P) {
  C3dTexEnv& T = L.texenv;
  if (P.texenv.size() > 6) { refuse(L, "more than 6 texenv stages"); return false; }
  T.used = int(P.texenv.size());
  for (int i = 0; i < 6; i++) T.stage[i] = i < T.used ? P.texenv[size_t(i)] : kPassThrough;
  T.updateBuffer = P.texenvUpdateBuffer & 0xFF00u;
  if (P.texenvUpdateBuffer != T.updateBuffer) { refuse(L, "texenv buffer update word has bits outside 8..15"); return false; }
  T.bufferColor = 0;
  return true;
}

// texture units: one TextureCache::get per non-scratch unit, in unit order
bool planUnits(C3dPlan& L, const DrawPacket& P, TextureCache& texCache, const GuestMem& mem, TexEntry* scratchView, bool diagnostics) {
  for (int u = 0; u < P.pica().numUnits && u < 3; u++) {
    const SubmitUnit& SU = P.units[u];
    if (SU.scratch) {   // the second pass of a two-pass draw reads the first pass's result: one texel per fragment, no filtering, no mip chain
      if (u != 0 || !P.pica().vs.tc0Projective) { refuse(L, "the scratch read must be texture unit 0 with the SCREEN texgen"); return false; }
      if (!scratchView || !scratchView->tex.ok) { refuse(L, "texture unit 0: the scratch target is not available"); return false; }
      C3dUnit& U = L.units[u];
      U.enabled = true;
      U.entry = scratchView;
      U.projective = true;
      U.wrapS = U.wrapT = ctru::CLAMP_TO_EDGE;
      U.magFilter = U.minFilter = U.mipFilter = ctru::NEAREST;
      U.minLevel = U.maxLevel = 0;
      U.lodBias = 0;
      continue;
    }
    if (SU.texmap < 0 || !SU.tex.valid) { refuse(L, "texture unit " + std::to_string(u) + ": no image bound"); return false; }
    TexMapOptions mo;
    mo.needAlpha = SU.needAlpha;
    for (int i = 0; i < g_benchRepeat[7]; i++) { volatile bool v = texCache.get(SU.tex, mem, mo).entry != nullptr; (void)v; }
    TexLookup lk = texCache.get(SU.tex, mem, mo);
    for (const std::string& n : lk.notes) { if(diagnostics) L.notes.push_back("texture unit " + std::to_string(u) + ": " + n); }
    if (!lk.entry || !lk.error.empty()) { refuse(L, "texture unit " + std::to_string(u) + ": " + (lk.error.empty() ? std::string("no texture") : lk.error)); return false; }   // (a cached failed conversion sets error too)
    C3dUnit& U = L.units[u];
    U.enabled = true;
    U.entry = lk.mutableEntry();
    U.projective = u == 0 && P.pica().vs.tc0Projective;
    const PicaSamplerDesc& s = lk.sampler;
    U.wrapS = wrapFor(s.wrapS); U.wrapT = wrapFor(s.wrapT);
    U.magFilter = s.magLinear ? ctru::LINEAR : ctru::NEAREST;
    U.minFilter = s.minLinear ? ctru::LINEAR : ctru::NEAREST;
    U.mipFilter = s.mipFilter == 2 ? ctru::LINEAR : ctru::NEAREST;
    U.minLevel = uint8_t(s.minLevel); U.maxLevel = uint8_t(s.maxLevel);   // samplerFor clamps them to the stored chain and leaves 0 / 0 without mip filtering
    // The PICA measures the footprint in its own texels per target pixel, the GX unit in the image's texels per EFB pixel: the frame scale (P.lodFrameBias, -1 at half
    // resolution) and the entry's texel density (padded / stretched axes, rendered copies at the port's resolution) are the difference, so the same bias moves the PICA's LOD
    // back to the GX value, for the mip level, the trilinear weight and the choice between the magnification and minification filter.
    float density = 1.0f;
    {
      const MappedTexture& m = U.entry->tex;
      const GxTexBinding& b = SU.tex;
      if (m.width > 0 && m.height > 0 && b.width > 0 && b.height > 0 && m.scaleS > 0 && m.scaleT > 0)
        density = std::sqrt(m.scaleS * float(m.width) / float(b.width) * m.scaleT * float(m.height) / float(b.height));
    }
    // Most bindings have unit texel density. log2(1) is exactly zero;
    // avoid a software transcendental per texture without changing mip selection.
    const float densityBias = density == 1.0f ? 0.0f : std::log2(density);
    U.lodBias = c3dLodBias(s.lodBias + P.lodFrameBias - densityBias);
  }
  return true;
}

// texture scale: the packet's texScale rows assume the image the binding describes; an entry may be laid out
// differently (a rendered EFB copy fills a different fraction of its power-of-two texture), so the rows follow the entry
void planTexScale(C3dPlan& L, const DrawPacket& P, bool diagnostics) {
  bool patched = false;
  for (int u = 0; u < P.pica().numUnits && u < 3; u++) {
    const C3dUnit& U = L.units[u];
    if (!U.enabled || P.units[u].scratch) continue;
    const float es = U.entry->tex.scaleS, et = U.entry->tex.scaleT;
    if (std::fabs(es - P.units[u].scaleS) < 1e-6f && std::fabs(et - P.units[u].scaleT) < 1e-6f) continue;
    const int slot = P.pica().vs.texScaleSlot[u];
    C3dUniform* tu = nullptr;
    for (C3dUniform& cu : L.uniforms) if (*cu.name == "texScale") tu = &cu;
    if (slot < 0 || !tu || slot >= tu->count) {
      { if(diagnostics) L.notes.push_back("texture unit " + std::to_string(u) + ": the entry fills " + std::to_string(es) + " x " + std::to_string(et) + " of its texture but the shader has no texScale for it: the picture is mis-scaled"); }
      continue;
    }
    if (!patched) { L.ownedRows.assign(tu->rows, tu->rows + tu->count); tu->rows = L.ownedRows.data(); patched = true; }
    L.ownedRows[size_t(slot)].v[0] = es; L.ownedRows[size_t(slot)].v[1] = et;
  }
}

// fixed-function state (may append the destination-alpha texenv stage)
void planEffects(C3dPlan& L, const DrawPacket& P, bool diagnostics) {
  C3dEffect& E = L.fx;
  E.zBuffer = P.frame.depth.zBuffer; E.zScale = P.frame.depth.scale; E.zOffset = P.frame.depth.offset;
  E.cull = uint8_t(P.frame.picaCull);
  const bool hasDstAlpha = P.pixelFmt == 1;          // RGBA6_Z24: the only GX target format that stores alpha; the others read 1.0 from the destination
  E.depthTest = P.pica().depth.testEnable; E.depthFunc = P.pica().depth.func;
  E.writeMask = uint8_t((P.colorUpdate ? ctru::WRITE_RED | ctru::WRITE_GREEN | ctru::WRITE_BLUE : 0) | (P.alphaUpdate && hasDstAlpha ? ctru::WRITE_ALPHA : 0) |
                        (P.pica().depth.write && P.pica().depth.testEnable ? ctru::WRITE_DEPTH : 0));
  if (P.pica().role == PassRole::ScratchWrite)   // the scratch holds the whole RGBA result whatever the draw's masks and the EFB format say; the draw's masks apply in the second pass
    E.writeMask = uint8_t(ctru::WRITE_RED | ctru::WRITE_GREEN | ctru::WRITE_BLUE | ctru::WRITE_ALPHA | (P.pica().depth.write && P.pica().depth.testEnable ? ctru::WRITE_DEPTH : 0));
  E.alphaTest = P.pica().alpha.enable; E.alphaFunc = P.pica().alpha.func; E.alphaRef = P.pica().alpha.ref;
  E.logicOp = P.pica().blend.useLogicOp; E.logic = P.pica().blend.logicOp;
  E.eqRgb = P.pica().blend.eqRgb; E.eqAlpha = P.pica().blend.eqAlpha;
  E.srcRgb = P.pica().blend.srcRgb; E.dstRgb = P.pica().blend.dstRgb; E.srcAlpha = P.pica().blend.srcAlpha; E.dstAlpha = P.pica().blend.dstAlpha;
  E.blendColor = 0;
  E.fog = P.pica().fog.enable; E.fogColor = P.pica().fog.color;
  if (E.fog) std::memcpy(L.fogLut, P.pica().fog.lut, sizeof(L.fogLut));
  if (P.pixelFmt != 0 && P.pixelFmt != 1) { if(diagnostics) L.notes.push_back("EFB pixel format " + std::to_string(P.pixelFmt) + ": rendered as RGB8 / Z24"); }
  // GX destination alpha (GXSetDstAlpha): the alpha the EFB receives is the constant, whatever the blend does to it; the blend's colour factors and the alpha test still see the
  // TEV alpha. A constant of 0 is exact on the PICA: the alpha half of the blend becomes 0 * src + 0 * dst (games use it to reset the alpha channel, e.g. Wind Waker's shadow mask).
  if (P.dstAlphaEnable && hasDstAlpha && P.alphaUpdate && P.pica().role != PassRole::ScratchWrite) {
    // Any other constant: when nothing else reads the TEV alpha (no alpha test, the colour blend does not use source alpha), a texenv stage appended after the
    // material's last one passes the colour through and outputs the constant as alpha, and the alpha half of the blend writes it unchanged (ONE, ZERO).
    const auto srcAlphaFactor = [](uint8_t f) { return f == 6 || f == 7 || f == 14; };   // SRC_ALPHA, ONE_MINUS_SRC_ALPHA, SRC_ALPHA_SATURATE
    if (E.logicOp) { if(diagnostics) L.notes.push_back("destination alpha constant " + std::to_string(P.dstAlpha) + " is not applied (logic op): the alpha channel receives the TEV alpha"); }
    else if (P.dstAlpha == 0) { E.eqAlpha = 0; E.srcAlpha = 0; E.dstAlpha = 0; }
    else if (!E.alphaTest && !srcAlphaFactor(E.srcRgb) && !srcAlphaFactor(E.dstRgb) && L.texenv.used < 6) {
      HwStage& st = L.texenv.stage[L.texenv.used++];
      st.sources = 0x000Fu | (0x000Eu << 16);   // rgb: PREVIOUS, alpha: CONSTANT
      st.modifiers = 0; st.ops = 0;              // operands as they are, REPLACE
      st.constColor = uint32_t(P.dstAlpha) << 24;
      st.scales = 0;
      E.eqAlpha = 0; E.srcAlpha = 1; E.dstAlpha = 0;
    } else { if(diagnostics) L.notes.push_back("destination alpha constant " + std::to_string(P.dstAlpha) + " is not applied: the alpha channel receives the TEV alpha"); }
  }
  if (P.dither) { if(diagnostics) L.notes.push_back("dithering is not applied"); }
}

// benchmark knobs at the end of a successful plan (both paths, so the repeat measurements keep their meaning)
void planBenchTail(C3dPlan& L, const DrawPacket& P, C3dShaderCache& shaders, TextureCache& texCache, const GuestMem& mem, TexEntry* scratchView, bool diagnostics) {
  for (int i = 0; i < g_benchRepeat[0]; i++) c3dFinalize(L);
  for (int i = 0; i < g_benchRepeat[1]; i++) { volatile uint32_t v = C3dPlan().fogLut[3]; (void)v; }
  if (g_benchRepeat[6] > 0) {   // extra full builds into a scratch plan (the knob is cleared meanwhile so they do not recurse)
    static C3dPlan extra; const int r = g_benchRepeat[6]; g_benchRepeat[6] = 0;
    for (int i = 0; i < r; i++) buildPlanInto(extra, P, shaders, texCache, mem, scratchView,diagnostics);
    g_benchRepeat[6] = r;
  }
}

}  // namespace

// ================================================================ plan-template cache
struct C3dPlanTemplateCache::Impl {
  static constexpr uint32_t kKeyWords = 128;   // 117 used at most (12 attributes, 6 texenv stages)
  static constexpr size_t kAlloc = 16;         // assumed allocator overhead per heap block (accounting only)
  struct Desc { const std::string* name = nullptr; int id = 0; int size = 0; int slot = -1; };
  struct Entry {
    uint64_t hash = 0;
    const PicaDraw* pica = nullptr;            // compared together with the owner's control block, never dereferenced
    std::weak_ptr<const PicaDraw> owner;       // weak: never extends the compiled draw's life
    const C3dShader* shader = nullptr;         // the shader the template was built with (checked against shaders.get on every hit)
    std::vector<uint32_t> key;                 // the complete serialized key (collisions compare it whole)
    C3dAttrs attrs;
    std::vector<Desc> descs;                   // uniform descriptors in register order (all packed)
    C3dTexEnv texenv;                          // FINAL texenv (after the effect block's destination-alpha stage, if any)
    C3dEffect fx;
    std::vector<uint32_t> fogLut;              // 128 words when fx.fog, empty otherwise
    uint64_t hashAttrs = 0, hashTexEnv = 0, hashEffect = 0;
    size_t bytes = 0;
  };
  using List = std::list<Entry>;
  List lru;                                                   // most recently used first
  std::unordered_multimap<uint64_t, List::iterator> byHash;   // buckets reserved for the most entries the budget can hold: no rehash after setBudget
  List::iterator mruIt; bool mruValid = false;
  // query scratch (filled by makeKey, read by find / insert within one buildPlanInto call; fixed storage, no allocation)
  uint32_t q[kKeyWords] = {}; uint32_t qn = 0; uint64_t qh = 0; const PicaDraw* qp = nullptr; std::weak_ptr<const PicaDraw> qOwner;
  size_t budget = 0, bytes = 0, fixedBytes = 0;
  C3dPlanTemplateStats st;

  static size_t minEntryBytes() { return sizeof(Entry) + 2 * sizeof(void*) + kAlloc + sizeof(std::pair<const uint64_t, List::iterator>) + 2 * sizeof(void*) + kAlloc; }
  static size_t entryBytes(const Entry& e) {
    size_t b = minEntryBytes();
    b += e.key.capacity() * sizeof(uint32_t) + kAlloc;
    b += e.descs.capacity() * sizeof(Desc) + kAlloc;
    if (e.fogLut.capacity()) b += e.fogLut.capacity() * sizeof(uint32_t) + kAlloc;
    return b;
  }
  size_t room() const { return budget > fixedBytes ? budget - fixedBytes : 0; }
  void setBudget(size_t b) {
    budget = b;
    byHash.reserve(b / minEntryBytes() + 1);
    fixedBytes = sizeof(C3dPlanTemplateCache::Impl) + byHash.bucket_count() * sizeof(void*) + kAlloc;
    while (bytes > room() && !lru.empty()) { drop(std::prev(lru.end())); st.evictions++; }
  }
  void dropList(List::iterator li) {
    bytes -= std::min(bytes, li->bytes);
    if (mruValid && mruIt == li) mruValid = false;
    lru.erase(li);
  }
  void drop(List::iterator li) {
    auto range = byHash.equal_range(li->hash);
    for (auto it = range.first; it != range.second; ++it) if (it->second == li) { byHash.erase(it); break; }
    dropList(li);
  }
  void clear() { lru.clear(); byHash.clear(); bytes = 0; mruValid = false; }
  void purgeExpired() {
    for (auto li = lru.begin(); li != lru.end();) { auto next = std::next(li); if (li->owner.expired()) { drop(li); st.expired++; } li = next; }
  }

  // Everything that determines the cached part, serialized field by field (no struct bytes). The PicaDraw identity (owner + address) is kept beside it.
  bool makeKey(const DrawPacket& P) {
    qn = 0;
    if (P.layout.size() > 12 || P.texenv.size() > 6) return false;   // the baseline refuses these; never cached
    auto w = [this](uint32_t v) { q[qn++] = v; };
    auto f = [&w](float v) { uint32_t x; std::memcpy(&x, &v, 4); w(x); };
    w(1u);   // key format version
    w(uint32_t(P.layout.size()));
    // (Claude v77) planar offsets follow from the layout and the vertex count: not keyed, set from the packet on a hit
    for (const SubmitAttr& a : P.layout) { w(uint32_t(a.kind)); w(uint32_t(a.index)); w(uint32_t(a.reg)); w(uint32_t(a.components)); w(a.ubyte ? 1u : 0u); w(P.planar ? 0u : a.offset); }
    w(P.stride); w(P.planar ? 1u : 0u);
    w(uint32_t(P.texenv.size()));
    for (const HwStage& s : P.texenv) { w(s.sources); w(s.modifiers); w(s.ops); w(s.constColor); w(s.scales); }
    w(P.texenvUpdateBuffer);
    w(P.frame.depth.zBuffer ? 1u : 0u); f(P.frame.depth.scale); f(P.frame.depth.offset); w(uint32_t(P.frame.picaCull));
    w(P.colorUpdate ? 1u : 0u); w(P.alphaUpdate ? 1u : 0u); w(P.pixelFmt); w(P.dstAlphaEnable ? 1u : 0u); w(P.dstAlpha); w(P.usesPackedUniforms ? 1u : 0u);
    qp = &P.pica();
    qOwner = P.picaOwner();
    uint64_t h = 0xcbf29ce484222325ull ^ uint64_t(reinterpret_cast<uintptr_t>(qp));
    for (uint32_t i = 0; i < qn; i++) { h = (h ^ q[i]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
#ifdef T2P_PLANCACHE_TEST_HASH
    h = T2P_PLANCACHE_TEST_HASH(h);   // test builds only (-D'T2P_PLANCACHE_TEST_HASH(h)=0'): every key collides, the full-key compare must decide
#endif
    qh = h;
    return true;
  }
  bool matches(const Entry& e) const {
    // same control block (a held weak_ptr keeps it from being reused) and still alive; the address and the whole key must match too
    return e.hash == qh && e.pica == qp && !e.owner.owner_before(qOwner) && !qOwner.owner_before(e.owner) && !e.owner.expired() &&
           e.key.size() == qn && std::memcmp(e.key.data(), q, size_t(qn) * sizeof(uint32_t)) == 0;
  }
  // The entry stays valid until the next insert / drop / clear: buildPlanInto only inserts after it has finished with a hit (never on the hit path).
  Entry* find() {
    if (mruValid && matches(*mruIt)) { lru.splice(lru.begin(), lru, mruIt); return &*mruIt; }
    auto range = byHash.equal_range(qh);
    for (auto it = range.first; it != range.second;) {
      const List::iterator li = it->second;
      if (li->owner.expired()) { it = byHash.erase(it); dropList(li); st.expired++; continue; }
      if (matches(*li)) { lru.splice(lru.begin(), lru, li); mruIt = li; mruValid = true; return &*li; }
      ++it;
    }
    return nullptr;
  }
  // after a successful full build of the queried packet
  void insert(const DrawPacket& P, const C3dPlan& L) {
    const C3dShader& sh = *L.shader;
    if (L.uniforms.size() != sh.uniformsById.size()) { st.rejected++; return; }
    for (const auto& kv : sh.uniformsById) if (kv.second.packedSlot < 0) { st.rejected++; return; }
    for (auto range = byHash.equal_range(qh); range.first != range.second; ++range.first)   // an exact-key entry built with another shader pointer: replace it
      if (matches(*range.first->second)) { drop(range.first->second); break; }
    Entry e;
    e.hash = qh; e.pica = qp; e.owner = qOwner; e.shader = L.shader;
    e.key.assign(q, q + qn);
    e.attrs = L.attrs;
    e.descs.reserve(sh.uniformsById.size());
    for (const auto& kv : sh.uniformsById) { Desc d; d.name = kv.second.name; d.id = kv.second.id; d.size = kv.second.size; d.slot = kv.second.packedSlot; e.descs.push_back(std::move(d)); }
    e.texenv = L.texenv; e.fx = L.fx;
    if (L.fx.fog) e.fogLut.assign(L.fogLut, L.fogLut + 128);
    e.hashAttrs = L.hashAttrs; e.hashTexEnv = L.hashTexEnv; e.hashEffect = L.hashEffect;
    e.bytes = entryBytes(e);
    (void)P;
    if (e.bytes > room()) { st.rejected++; return; }
    while (bytes + e.bytes > room() && !lru.empty()) { drop(std::prev(lru.end())); st.evictions++; }
    lru.push_front(std::move(e));
    byHash.emplace(qh, lru.begin());
    bytes += lru.front().bytes;
    mruIt = lru.begin(); mruValid = true;
    st.inserts++;
  }
};

C3dPlanTemplateCache::C3dPlanTemplateCache(size_t budgetBytes) : impl_(new Impl) { impl_->setBudget(budgetBytes); }
C3dPlanTemplateCache::~C3dPlanTemplateCache() = default;
void C3dPlanTemplateCache::clear() { impl_->clear(); }
void C3dPlanTemplateCache::setBudget(size_t budgetBytes) { impl_->setBudget(budgetBytes); }
void C3dPlanTemplateCache::purgeExpired() { impl_->purgeExpired(); }
C3dPlanTemplateStats C3dPlanTemplateCache::stats() const {
  C3dPlanTemplateStats s = impl_->st;
  s.entries = impl_->lru.size(); s.bytes = impl_->bytes + impl_->fixedBytes; s.budget = impl_->budget;
  return s;
}

// ================================================================ the plan
C3dPlan& buildPlanInto(C3dPlan& L,const DrawPacket& P, C3dShaderCache& shaders, TextureCache& texCache, const GuestMem& mem, TexEntry* scratchView, bool diagnostics,
                       C3dPlanTemplateCache* templates) {
  uint64_t profT = t2pProfTicks();
  auto profMark = [&profT](int slot) { if (t2pProfEnabled()) { const uint64_t t = t2pProfTicks(); g_t2pProf[slot] += t - profT; profT = t; } };
  struct ProfRest { uint64_t* t; ~ProfRest() { if (t2pProfEnabled()) g_t2pProf[14] += t2pProfTicks() - *t; } } profRest{&profT};
  // Plans are consumed immediately by the single GPU owner. Reuse container
  // capacity, but reset every value and borrowed pointer before the next draw.
  auto uniforms=std::move(L.uniforms);auto owned=std::move(L.ownedRows);auto notes=std::move(L.notes);
  L=C3dPlan();uniforms.clear();owned.clear();notes.clear();
  L.uniforms=std::move(uniforms);L.ownedRows=std::move(owned);L.notes=std::move(notes);
  if (!P.ok) return refuse(L, "the packet is not drawable");
  if (P.indexCount()==0) return refuse(L, "no indices");
  L.scissor = P.frame.scissorWindow;
  if (L.scissor.empty()) return refuse(L, "scissor rectangle is empty: nothing visible");

  // ---------------------------------------------------------------- plan template lookup (touches nothing but the template cache)
  C3dPlanTemplateCache::Impl* tc = nullptr;
  const C3dPlanTemplateCache::Impl::Entry* T = nullptr;
  if (templates) {
    C3dPlanTemplateCache::Impl& I = templates->impl();
    if (!diagnostics && P.usesPackedUniforms && P.packedUniforms && !P.picaOwner().expired() && I.makeKey(P)) { tc = &I; I.st.lookups++; T = I.find(); }
    else I.st.bypass++;
  }

  // ---------------------------------------------------------------- shader
  L.shader = shaders.get(P.pica());
  if (!L.shader->error.empty()) return refuse(L, "shader: " + L.shader->error);
  if (T && T->shader != L.shader) { tc->st.shaderMismatch++; T = nullptr; }
  if (tc) { if (T) tc->st.hits++; else tc->st.misses++; }

  if (T) {
    // ---------------------------------------------------------------- template hit: the baseline order, dynamic parts only
    // attributes: the key holds the layout, stride and (through the shader check) the input mask the template's loaders were validated against
    L.attrs = T->attrs;
    if (P.planar) for (int i = 0; i < L.attrs.count; i++) L.attrs.offset[i] = P.layout[size_t(i)].offset;
    if (!planGeometry(L, P)) return L;
    profMark(11);
    // uniforms: descriptors from the template, rows (and their counts) from this packet
    L.uniforms.reserve(T->descs.size());
    for (const auto& d : T->descs) {
      const auto& rows = P.packedUniforms->arrays[d.slot];
      if (rows.count != d.size) return refuse(L, "uniform " + *d.name + ": packed row count differs from declaration");
      C3dUniform u; u.name = d.name; u.id = d.id; u.rows = rows.rows; u.count = rows.count; L.uniforms.push_back(std::move(u));
    }
    profMark(12);
    if (!planTexEnv(L, P)) return L;
    if (!planUnits(L, P, texCache, mem, scratchView, diagnostics)) return L;
    profMark(13);
    planTexScale(L, P, diagnostics);
    // only now the immutable result (as the baseline assigns effects / hashes only after the units succeeded)
    L.texenv = T->texenv;
    L.fx = T->fx;
    if (L.fx.fog) std::memcpy(L.fogLut, T->fogLut.data(), sizeof(L.fogLut));
    L.hashAttrs = T->hashAttrs; L.hashTexEnv = T->hashTexEnv; L.hashEffect = T->hashEffect;
    planBenchTail(L, P, shaders, texCache, mem, scratchView, diagnostics);
    L.ok = true;
    return L;
  }

  // ---------------------------------------------------------------- vertex attributes
  {
    C3dAttrs& A = L.attrs;
    if (P.layout.empty() || P.layout.size() > 12) return refuse(L, "attribute count " + std::to_string(P.layout.size()) + " (1..12)");
    uint32_t off = 0, regs = 0;
    for (const SubmitAttr& sa : P.layout) {
      if (sa.reg < 0 || sa.reg > 15 || (regs >> sa.reg & 1)) return refuse(L, "attribute input register " + std::to_string(sa.reg));
      if (sa.components < 1 || sa.components > 4) return refuse(L, "attribute component count");
      if (!P.planar && sa.offset != off) return refuse(L, "attribute offset " + std::to_string(sa.offset) + ", the previous attributes end at " + std::to_string(off));
      if (P.planar && sa.offset != off * P.vertexCount) return refuse(L, "planar attribute offset " + std::to_string(sa.offset));
      A.offset[A.count] = sa.offset; A.size[A.count] = uint32_t(sa.components) * (sa.ubyte ? 1u : 4u);
      regs |= 1u << sa.reg;
      C3dLoader& ld = A.loader[A.count];
      ld.reg = sa.reg;
      ld.format = sa.ubyte ? ctru::UNSIGNED_BYTE : ctru::FLOAT;
      ld.count = sa.components;
      A.permutation |= uint64_t(A.count) << (4 * A.count);
      off += uint32_t(sa.components) * (sa.ubyte ? 1u : 4u);
      A.count++;
    }
    if (off != P.stride) return refuse(L, "stride " + std::to_string(P.stride) + " differs from the attribute sizes " + std::to_string(off));
    if (P.stride % 4) return refuse(L, "stride " + std::to_string(P.stride) + " is not a multiple of 4");   // 12 attributes of at most 16 bytes stay below the 8 bit limit
    A.stride = P.stride; A.planar = P.planar;
    if (P.planar) for (int i = 0; i < A.count; i++) if (A.size[i] % 4) return refuse(L, "planar attribute size");
    if ((L.shader->inputMask & ~regs) != 0) return refuse(L, "the shader reads input registers the packet does not supply");
    if (popcount16(regs & ~unsigned(L.shader->inputMask)) != 0) { if(diagnostics) L.notes.push_back("the packet supplies input registers the shader does not read"); }
    if (!planGeometry(L, P)) return L;
  }

  profMark(11);
  // ---------------------------------------------------------------- uniforms: every float uniform the shader declares gets its rows (a missing one would keep a stale value in the register)
  L.uniforms.reserve(L.shader->uniformsById.size());
  for (const auto& kv : L.shader->uniformsById) {   // (in register order)
    if(P.usesPackedUniforms && P.packedUniforms && kv.second.packedSlot>=0) {
      const auto& rows=P.packedUniforms->arrays[kv.second.packedSlot];
      if(rows.count!=kv.second.size)return refuse(L,"uniform "+kv.first+": packed row count differs from declaration");
      C3dUniform u;u.name=kv.second.name;u.id=kv.second.id;u.rows=rows.rows;u.count=rows.count;L.uniforms.push_back(std::move(u));continue;
    }
    auto it = P.uniforms.find(kv.first);
    if (it == P.uniforms.end()) return refuse(L, "uniform " + kv.first + " has no value");
    if (int(it->second.size()) != kv.second.size) return refuse(L, "uniform " + kv.first + ": " + std::to_string(it->second.size()) + " rows for a declaration of " + std::to_string(kv.second.size));
    C3dUniform u;
    u.name = kv.second.name; u.id = kv.second.id; u.rows = it->second.data(); u.count = int(it->second.size());
    L.uniforms.push_back(u);
  }
  // Packets carry every uniform array of the host contract (vtx.h); a shader declares a subset and the rest is simply not uploaded. Only names outside the contract are noted.
  for (const auto& kv : P.uniforms) {
    static const std::string_view kContract[] = {"projMtx", "posMtx", "nrmMtx", "matColor", "ambColor", "light", "texMtx", "texScale", "dbgScale"};
    bool known = false;
    for (std::string_view c : kContract) if (kv.first == c) { known = true; break; }
    if (!known && !L.shader->uniforms.count(kv.first)) { if(diagnostics) L.notes.push_back("packet uniform " + kv.first + " is not declared by the shader: not uploaded"); }
  }

  profMark(12);
  // ---------------------------------------------------------------- texenv
  if (!planTexEnv(L, P)) return L;
  // ---------------------------------------------------------------- texture units
  if (!planUnits(L, P, texCache, mem, scratchView, diagnostics)) return L;
  profMark(13);
  // ---------------------------------------------------------------- texture scale
  planTexScale(L, P, diagnostics);
  profMark(14);
  // ---------------------------------------------------------------- fixed-function state
  planEffects(L, P, diagnostics);
  profMark(7);   // (Claude v77 profiling: render-side slot 7 = planEffects)

  c3dFinalize(L);
  planBenchTail(L, P, shaders, texCache, mem, scratchView, diagnostics);
  L.ok = true;
  if (tc) tc->insert(P, L);   // only a successful full build becomes a template (its query key is still in tc's scratch)
  return L;
}

bool buildPlanDelta(C3dPlan& L, const DrawPacket& P, bool diagnostics) {
  if (!L.ok || !P.ok || P.indexCount() == 0) return false;
  const PixelRect& w = P.frame.scissorWindow;
  if (w.empty() || w.x0 != L.scissor.x0 || w.y0 != L.scissor.y0 || w.x1 != L.scissor.x1 || w.y1 != L.scissor.y1) return false;
  if (P.stride != L.attrs.stride || P.planar != L.attrs.planar || int(P.layout.size()) != L.attrs.count) return false;
  if (P.planar && L.vertexBytes != P.vertexBytes()) return false;   // planar offsets scale with the vertex count
  for (int i = 0; i < L.attrs.count; i++) {   // the loaders the full build made from this layout
    const SubmitAttr& sa = P.layout[size_t(i)];
    if (sa.offset != L.attrs.offset[i] || sa.reg != L.attrs.loader[i].reg || sa.components != L.attrs.loader[i].count ||
        (sa.ubyte ? ctru::UNSIGNED_BYTE : ctru::FLOAT) != L.attrs.loader[i].format) return false;
  }
  if (!P.usesPackedUniforms || !P.packedUniforms || L.uniforms.size() != L.shader->uniformsById.size()) return false;
  L.linearGeometry = false;
  L.notes.clear();
  if (!planGeometry(L, P)) return false;
  // uniform rows of THIS packet (same declarations, same order as the full build), then the same rendered-copy texScale patch on them
  {
    size_t k = 0;
    for (const auto& kv : L.shader->uniformsById) {
      if (kv.second.packedSlot < 0) return false;
      const auto& rows = P.packedUniforms->arrays[kv.second.packedSlot];
      if (rows.count != kv.second.size) return false;
      C3dUniform& u = L.uniforms[k++];
      if (u.id != kv.second.id) return false;
      u.rows = rows.rows; u.count = rows.count;
    }
  }
  planTexScale(L, P, diagnostics);
  if (!planTexEnv(L, P)) return false;
  planEffects(L, P, diagnostics);
  c3dFinalize(L);
  return true;
}

C3dPlan buildPlan(const DrawPacket& P,C3dShaderCache& shaders,TextureCache& cache,const GuestMem& mem,TexEntry* scratch,bool diagnostics,C3dPlanTemplateCache* templates) {
  C3dPlan plan;buildPlanInto(plan,P,shaders,cache,mem,scratch,diagnostics,templates);return plan;
}

}  // namespace t2p
