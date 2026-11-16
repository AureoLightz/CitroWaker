// (Claude v73) Cross-frame draw reuse for Renderer3ds::drawOne.
// A draw whose compiled shader (DrawCompileCache identity), non-uniform state and decoded geometry equal those of a draw seen in an earlier frame
// reuses that draw's packet: its layout, texenv words, texture units and its packed vertex / index buffers, which stay in linear (GPU) memory.
// Per hit only the frame mapping (mapFrame: it reads the projection matrix and, for 2D draws, the position matrices) and the uniforms are made again,
// exactly as fillPacket makes them. Everything fillPacket reads besides the uniforms is in the key:
//   - S.draw through the PicaDraw pointer (DrawCompileCache returns one object per equal GxDrawState; the entry holds it alive);
//   - every other GxState field except notes and the uniform blocks, serialised field by field (no padding bytes), compared exactly;
//   - the geometry: vertex count, attribute flags, every present attribute array, the triangle list, and for a matrix group every member's geometry and
//     slot map; two independent 64-bit hashes (128 bits) plus the counts.
// FrameConfig / Options are fixed for the renderer's life; Renderer3ds::setDrawReuse(false) drops the cache.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#include "submit.h"
#include "gxstate.h"
#include "matrixbatch.h"

namespace t2p {
namespace reuse {

struct Hash2 {
  uint64_t a = 0x243F6A8885A308D3ull, b = 0x13198A2E03707344ull;
  // 32-bit lanes keep the ARM11 inner loop to 32x32 multiplies; four lanes, combined at the end into a and b.
  uint32_t h0 = 0x9E3779B9u, h1 = 0x85EBCA6Bu, h2 = 0xC2B2AE35u, h3 = 0x27D4EB2Fu;
  static inline uint32_t rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
  inline void word(uint32_t w) {
    h0 = rotl(h0 ^ (w * 0xCC9E2D51u), 15) * 0x1B873593u;
    h1 = rotl(h1 + (w ^ 0xE6546B64u), 13) * 0x85EBCA6Bu;
    h2 = (h2 ^ rotl(w, 7)) * 0xC2B2AE35u + 0x9E3779B9u;
    h3 = rotl(h3 + w * 0x27D4EB2Fu, 11) ^ (h3 >> 3);
  }
  void bytes(const void* p, size_t n) {
    const uint8_t* s = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) { uint32_t w; std::memcpy(&w, s + i, 4); word(w); }
    uint32_t t = 0; int k = 0;
    for (; i < n; i++, k++) t |= uint32_t(s[i]) << (8 * k);
    word(t ^ uint32_t(n) << 24);
    word(uint32_t(n));
  }
  void finish() {
    a ^= (uint64_t(h0) << 32 | h1); a *= 0xFF51AFD7ED558CCDull; a ^= a >> 33;
    b ^= (uint64_t(h2) << 32 | h3); b *= 0xC4CEB9FE1A85EC53ull; b ^= b >> 29;
    a ^= b * 0x9E3779B97F4A7C15ull;
  }
};

inline void hashVertices(Hash2& h, const GxDraw& d) {
  const GxVertices& V = d.v;
  uint32_t flags = uint32_t(V.hasPos) | uint32_t(V.hasNrm) << 1 | uint32_t(V.hasClr[0]) << 2 | uint32_t(V.hasClr[1]) << 3 | uint32_t(V.hasPmIdx) << 4 | uint32_t(d.unsupportedPrim) << 5;
  for (int t = 0; t < 8; t++) flags |= uint32_t(V.hasTex[t]) << (8 + t) | uint32_t(V.hasTexMtxIdx[t]) << (16 + t);
  h.word(V.count); h.word(flags);
  if (V.hasPos) h.bytes(V.pos.data(), V.pos.size() * sizeof(float));
  if (V.hasNrm) h.bytes(V.nrm.data(), V.nrm.size() * sizeof(float));
  for (int c = 0; c < 2; c++) if (V.hasClr[c]) h.bytes(V.clr[c].data(), V.clr[c].size());
  for (int t = 0; t < 8; t++) if (V.hasTex[t]) h.bytes(V.tex[t].data(), V.tex[t].size() * sizeof(float));
  if (V.hasPmIdx) h.bytes(V.pmIdx.data(), V.pmIdx.size());
  for (int t = 0; t < 8; t++) if (V.hasTexMtxIdx[t]) h.bytes(V.texMtxIdx[t].data(), V.texMtxIdx[t].size());
  h.bytes(d.tris.data(), d.tris.size() * sizeof(uint32_t));
}

// Every GxState field fillPacket / mapFrame / widescreenXScale read, except S.draw (key: PicaDraw) and the uniforms (made per hit).
inline void serialiseState(std::vector<uint32_t>& o, const GxState& S) {
  auto f = [&o](float v) { uint32_t w; std::memcpy(&w, &v, 4); o.push_back(w); };
  auto u = [&o](uint32_t v) { o.push_back(v); };
  for (int i = 0; i < 4; i++) for (int c = 0; c < 4; c++) f(S.kcolor[i].v[c]);
  for (int i = 0; i < 4; i++) for (int c = 0; c < 4; c++) f(S.tevReg[i].v[c]);
  for (const GxTexBinding& t : S.tex) {
    u(t.valid); u(t.addr); u(uint32_t(t.width)); u(uint32_t(t.height)); u(uint32_t(t.fmt)); u(t.wrapS | t.wrapT << 8 | t.magLinear << 16 | t.minLinear << 17 | t.edgeLod << 18 | t.biasClamp << 19);
    u(t.mipMode | t.aniso << 8); f(t.lodBias); f(t.minLod); f(t.maxLod); u(t.tlutOffset); u(uint32_t(t.tlutFmt));
  }
  f(S.viewport.x); f(S.viewport.y); f(S.viewport.width); f(S.viewport.height); f(S.viewport.nearZ); f(S.viewport.farZ);
  u(uint32_t(S.scissor.x)); u(uint32_t(S.scissor.y)); u(uint32_t(S.scissor.width)); u(uint32_t(S.scissor.height));
  u(S.proj.ortho); for (float v : S.proj.p) f(v); f(S.proj.nearZ); f(S.proj.farZ); for (auto& r : S.proj.m) for (float v : r) f(v);
  u(S.cullMode | S.colorUpdate << 8 | S.alphaUpdate << 9 | S.dither << 10 | S.dstAlphaEnable << 11 | S.zCompBeforeTex << 12 | uint32_t(S.dstAlpha) << 16 | uint32_t(S.pixelFmt) << 24);
  u(S.zFmt16); u(uint32_t(S.numTexGens)); u(uint32_t(S.numChans)); u(uint32_t(S.numTevStages)); u(uint32_t(S.numIndStages));
  u(uint32_t(S.curPosMtx)); u(uint32_t(S.uniformPosSel));
}

struct Key {
  const PicaDraw* pica = nullptr;
  uint64_t geoA = 0, geoB = 0;
  uint32_t vertices = 0, indices = 0, members = 0;
  std::vector<uint32_t> state;
  uint64_t lookup() const {
    uint64_t k = geoA ^ uint64_t(uintptr_t(pica)) * 0x9E3779B97F4A7C15ull;
    for (uint32_t w : state) k = (k ^ w) * 0x100000001B3ull;
    return k ? k : 1;
  }
  bool operator==(const Key& o) const {
    return pica == o.pica && geoA == o.geoA && geoB == o.geoB && vertices == o.vertices && indices == o.indices && members == o.members && state == o.state;
  }
};

inline void makeKey(Key& k, const PicaDraw* pica, const GxState& S, const GxDraw& d, const matrixbatch::Group* group) {
  k.pica = pica;
  Hash2 h;
  if (group) {
    k.members = uint32_t(group->count); k.vertices = group->vertices; k.indices = 0;
    h.word(uint32_t(group->paletteSize)); h.word(uint32_t(group->count));
    for (size_t m = 0; m < group->count; m++) {
      hashVertices(h, *group->draws[m]);
      h.bytes(group->slots[m].data(), group->slots[m].size());
      k.indices += uint32_t(group->draws[m]->tris.size());
    }
  } else {
    k.members = 1; k.vertices = d.v.count; k.indices = uint32_t(d.tris.size());
    hashVertices(h, d);
  }
  h.finish(); k.geoA = h.a; k.geoB = h.b;
  k.state.clear(); serialiseState(k.state, S);
}

// (Claude v83) translate-once: the geometry of retained draws (gxretain.h) is named by RetainedGeometry::id (unique per immutable decoded geometry, never
// reused), so the key takes the members' ids (and a group's slot maps) instead of hashing every decoded vertex. Same equality: equal ids = equal geometry.
inline void makeKeyIds(Key& k, const PicaDraw* pica, const GxState& S, const GxDraw& d, const matrixbatch::Group* group, const uint64_t* ids) {
  k.pica = pica;
  Hash2 h;
  h.word(0x1D5u);   // id keys never equal hashed-geometry keys
  if (group) {
    k.members = uint32_t(group->count); k.vertices = group->vertices; k.indices = 0;
    h.word(uint32_t(group->paletteSize)); h.word(uint32_t(group->count));
    for (size_t m = 0; m < group->count; m++) {
      h.word(uint32_t(ids[m])); h.word(uint32_t(ids[m] >> 32));
      h.bytes(group->slots[m].data(), group->slots[m].size());
      k.indices += uint32_t(group->draws[m]->tris.size());
    }
  } else {
    k.members = 1; k.vertices = d.v.count; k.indices = uint32_t(d.tris.size());
    h.word(uint32_t(ids[0])); h.word(uint32_t(ids[0] >> 32));
  }
  h.finish(); k.geoA = h.a; k.geoB = h.b;
  k.state.clear(); serialiseState(k.state, S);
}

struct Entry {
  Key key;
  DrawPacket packet;                 // externalVertices / externalIndices point at mem
  void* mem = nullptr; size_t bytes = 0;
  uint64_t lastFrame = 0;
};

}  // namespace reuse
}  // namespace t2p
