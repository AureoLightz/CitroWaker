#include "submit.h"
#include "matrixbatch.h"

#include <cmath>
#include "drawcache.h"
#include "texfit.h"

#include <algorithm>
#include <cstring>

namespace t2p {
bool g_packPlanar = false;   // (Claude v77) see fillPacket
bool g_packVertexMajor = false;   // (Claude) see fillPacket
namespace {

std::string num(int v) { return std::to_string(v); }

}  // namespace

const PicaDraw& DrawPacket::pica() const {
  static const PicaDraw empty;
  return pica_ ? *pica_ : empty;
}

PicaDraw& DrawPacket::picaMut() {
  if (!pica_) pica_ = std::make_shared<PicaDraw>();
  else if (pica_.use_count() > 1) pica_ = std::make_shared<PicaDraw>(*pica_);   // never change what the cache (or another packet) holds
  return const_cast<PicaDraw&>(*pica_);
}

namespace {

// Everything of a packet that follows from its PicaDraw (P.pica() is set): frame mapping, vertex buffer, uniforms, texenv words, units. One call per pass of a multipass draw.
void fillPacket(DrawPacket& P, const GxState& S, const GxDraw& d, const FrameConfig& frameCfg, bool notes = true, bool packed = false,
                PackedGeometryCache* packCache = nullptr, uint64_t geoId = 0, const PacketGeometryOutput* output = nullptr, const matrixbatch::Group* group = nullptr, std::shared_ptr<PackedUniforms> preparedUniforms = {}) {
  P.viewport = S.viewport; P.scissor = S.scissor; P.cullMode = S.cullMode;
  P.colorUpdate = S.colorUpdate; P.alphaUpdate = S.alphaUpdate; P.dither = S.dither;
  P.dstAlphaEnable = S.dstAlphaEnable; P.dstAlpha = S.dstAlpha; P.pixelFmt = S.pixelFmt; P.zCompBeforeTex = S.zCompBeforeTex;
  if (!P.pica().vs.ok) return;
  P.frame = mapFrame(S, frameCfg, P.pica().depthMap, widescreenXScale(S, d, frameCfg));
  for (int i = 0; i < g_benchRepeat[3]; i++) { volatile bool v = mapFrame(S, frameCfg, P.pica().depthMap, widescreenXScale(S, d, frameCfg)).ok; (void)v; }
  if (notes) for (const std::string& n : P.frame.notes) P.notes.push_back(n);
  // The final GPU plan already rejects this exact empty rectangle. Avoid
  // packing geometry/uniforms for pixels that cannot reach the framebuffer.
  if (!P.frame.ok || P.frame.scissorWindow.empty()) return;
  if (frameCfg.outW > 0 && frameCfg.outH > 0 && frameCfg.efbW > 0 && frameCfg.efbH > 0)
    P.lodFrameBias = 0.5f * (std::log2(frameCfg.outW / float(frameCfg.efbW)) + std::log2(frameCfg.outH / float(frameCfg.efbH)));

  // ---------------------------------------------------------------- vertex buffer
  const uint64_t pt0 = t2pProfTicks();
  const GxVertices& V = d.v;
  uint32_t off = 0;
  for (const DrawAttr& a : P.pica().attrs) {
    SubmitAttr sa;
    sa.kind = a.kind; sa.index = a.index; sa.reg = a.reg;
    switch (a.kind) {
      case DrawAttr::Pos: case DrawAttr::Nrm: sa.components = 3; sa.ubyte = false; break;
      case DrawAttr::Clr0: sa.components = 4; sa.ubyte = true; break;
      case DrawAttr::MtxIdx: sa.components = 4; sa.ubyte = true; break;
      case DrawAttr::Tex: sa.components = 2; sa.ubyte = false; break;
    }
    sa.offset = off;
    off += uint32_t(sa.components) * (sa.ubyte ? 1u : 4u);
    off = (off + 3) & ~3u;
    P.layout.push_back(sa);
  }
  P.stride = off;
  P.vertexCount = group ? group->vertices : V.count;
  // (Claude v77) planar: attribute i's array starts where the arrays before it end (each element a multiple of 4 bytes, so every array is 4-aligned)
  P.planar = g_packPlanar && !packCache && P.layout.size() > 1;
  if (P.planar) { uint32_t at = 0; for (SubmitAttr& sa : P.layout) { const uint32_t bytes = uint32_t(sa.components) * (sa.ubyte ? 1u : 4u); sa.offset = at; at += ((bytes + 3) & ~3u) * P.vertexCount; } }
  size_t indexCount=d.tris.size();
  if(group){indexCount=0;for(size_t m=0;m<group->count;m++)indexCount+=group->draws[m]->tris.size();}

  bool missingNrm = false, missingClr = false;
  bool reused = false;
  if (output && output->resident && output->resident->ids && output->resident->find && !packCache) {
    ResidentGeometryLookup& L = *output->resident; ResidentGeometryKey& K = L.key;
    K.ids.assign(L.ids, L.ids + L.idCount);
    K.slots.clear(); if (group) for (size_t m = 0; m < group->count; m++) K.slots.insert(K.slots.end(), group->slots[m].begin(), group->slots[m].end());
    K.sig.clear();
    for (const SubmitAttr& sa : P.layout) K.sig.push_back(uint32_t(sa.kind) | uint32_t(sa.index) << 4 | uint32_t(sa.reg) << 8 | uint32_t(sa.components) << 12 | uint32_t(sa.ubyte) << 15 | sa.offset << 16);
    K.stride = P.stride; K.vertexCount = P.vertexCount; K.indexCount = uint32_t(indexCount); K.planar = P.planar;
    uint64_t h = 0x84222325CBF29CE4ull ^ (uint64_t(P.stride) << 32 | P.vertexCount) ^ (uint64_t(indexCount) << 1 | uint64_t(P.planar));
    for (uint64_t id : K.ids) h = (h ^ id) * 0x100000001B3ull;
    for (int8_t sl : K.slots) h = (h ^ uint8_t(sl)) * 0x100000001B3ull;
    for (uint32_t w : K.sig) h = (h ^ w) * 0x100000001B3ull;
    L.hash = h ? h : 1; L.hit = false;
    ResidentGeometryHit H;
    if (L.find(L.context, L.hash, K, H)) {
      P.externalVertices = H.vertices; P.externalIndices = H.indices; P.externalIndexCount = indexCount;
      missingNrm = H.missingNrm; missingClr = H.missingClr; P.indicesChecked = H.indicesChecked;
      reused = true; L.hit = true;
    }
  }
  if (packCache && geoId) {
    P.packSig.clear();
    uint64_t key = geoId * 0x9E3779B97F4A7C15ull ^ P.stride;
    for (const SubmitAttr& sa : P.layout) {
      const uint32_t s = uint32_t(sa.kind) | uint32_t(sa.index) << 4 | uint32_t(sa.reg) << 8 | uint32_t(sa.components) << 12 | uint32_t(sa.ubyte) << 15 | sa.offset << 16;
      P.packSig.push_back(s);
      key = (key ^ s) * 0xFF51AFD7ED558CCDull;
    }
    if (!key) key = 1;
    PackedGeometryCache::Entry* e = packCache->find(key, geoId, P.stride, P.packSig);
    if (e && e->vertices.size() == size_t(P.stride) * V.count && e->indices.size() == d.tris.size()) {
      std::swap(P.vertices, e->vertices); std::swap(P.indices, e->indices);
      P.packedFrom = e; P.indicesChecked = e->indicesChecked;
      missingNrm = e->missingNrm; missingClr = e->missingClr;
      reused = true;
    } else {
      P.packKey = key; P.packGeoId = geoId;
    }
  }
  if (!reused) {
  // (Claude) no zero fill of the whole buffer first: every attribute slot is 4-byte aligned without gaps (offsets above), and each case below
  // writes its whole slot, zeros included (an absent attribute, the three unused bytes of a matrix index). Same bytes as assign(n, 0) + fill.
  if (output && output->allocate && !packCache) {
    uint8_t* vertices=nullptr; uint16_t* indices=nullptr;
    if(output->allocate(output->context,size_t(P.stride)*P.vertexCount,indexCount,vertices,indices)) {
      P.externalVertices=vertices; P.externalIndices=indices; P.externalIndexCount=indexCount;
    }
  }
  if(!P.externalVertices) P.vertices.resize(size_t(P.stride) * P.vertexCount);
  uint8_t* packedVertices=P.externalVertices?P.externalVertices:P.vertices.data();
  // one tight loop per attribute (the attribute's case decided once, not per vertex)
  const uint32_t stride = P.stride;
  if (P.planar) {
    // (Claude v77) each attribute's elements are consecutive: a present float/colour array is one block copy per member (the decoded arrays are
    // exactly count elements of the packed element size); absent attributes get the same constant bytes as the interleaved loops below.
    for (const SubmitAttr& sa : P.layout) {
      const uint32_t bytes = uint32_t(sa.components) * (sa.ubyte ? 1u : 4u);
      uint8_t* dst = packedVertices + sa.offset;
      for (size_t member = 0; member < (group ? group->count : 1); ++member) {
        const GxVertices& V = group ? group->draws[member]->v : d.v;
        const uint32_t count = V.count;
        switch (sa.kind) {
          case DrawAttr::Pos: if (V.hasPos) std::memcpy(dst, V.pos.data(), size_t(count) * 12); else std::memset(dst, 0, size_t(count) * 12); break;
          case DrawAttr::Nrm:
            if (V.hasNrm) std::memcpy(dst, V.nrm.data(), size_t(count) * 12);
            else { const float f[3] = {0, 0, 1}; missingNrm = true; for (uint32_t i = 0; i < count; i++) std::memcpy(dst + size_t(i) * 12, f, 12); }
            break;
          case DrawAttr::Clr0: if (V.hasClr[0]) std::memcpy(dst, V.clr[0].data(), size_t(count) * 4); else { missingClr = true; std::memset(dst, 255, size_t(count) * 4); } break;
          case DrawAttr::MtxIdx: {
            uint32_t* w = reinterpret_cast<uint32_t*>(dst);   // little endian: {index, 0, 0, 0}
            if (group) for (uint32_t i = 0; i < count; i++) { const unsigned k = V.hasPmIdx ? V.pmIdx[i] / 3 : 0; w[i] = uint32_t(uint8_t(3 * group->slots[member][k])); }
            else if (V.hasPmIdx) { const uint8_t* src = V.pmIdx.data(); for (uint32_t i = 0; i < count; i++) w[i] = src[i]; }
            else std::memset(dst, 0, size_t(count) * 4);
            break; }
          case DrawAttr::Tex: if (V.hasTex[sa.index]) std::memcpy(dst, V.tex[sa.index].data(), size_t(count) * 8); else std::memset(dst, 0, size_t(count) * 8); break;
        }
        dst += size_t(count) * bytes;
      }
    }
  } else
  if (g_packVertexMajor && P.layout.size() <= 16) {
    // (Claude) vertex-major: each vertex's slot is written once, front to back, so the stores stream through the ARM11's write buffer
    // (its L1 does not allocate on a write miss: the attribute-major passes below write every line of the buffer once per attribute).
    // Same bytes as the attribute-major loops: same offsets, same sources, same fills.
    struct VmAttr { uint32_t off; uint8_t bytes, mode; uint32_t step; const uint8_t* src; uint8_t k[12]; };   // mode 0 copy, 1 constant, 2 matrix index byte, 3 grouped matrix index
    VmAttr va[16]; size_t na = 0;
    uint32_t vertexBase = 0;
    for (size_t member = 0; member < (group ? group->count : 1); ++member) {
      const GxVertices& V = group ? group->draws[member]->v : d.v;
      const uint32_t count = V.count;
      na = 0;
      for (const SubmitAttr& sa : P.layout) {
        VmAttr& a = va[na++]; a.off = sa.offset; a.mode = 0; a.src = nullptr; a.step = 0; std::memset(a.k, 0, sizeof a.k);
        switch (sa.kind) {
          case DrawAttr::Pos: a.bytes = 12; if (V.hasPos) { a.src = reinterpret_cast<const uint8_t*>(V.pos.data()); a.step = 12; } else a.mode = 1; break;
          case DrawAttr::Nrm: a.bytes = 12; if (V.hasNrm) { a.src = reinterpret_cast<const uint8_t*>(V.nrm.data()); a.step = 12; } else { const float f[3] = {0, 0, 1}; std::memcpy(a.k, f, 12); a.mode = 1; missingNrm = true; } break;
          case DrawAttr::Clr0: a.bytes = 4; if (V.hasClr[0]) { a.src = V.clr[0].data(); a.step = 4; } else { std::memset(a.k, 255, 4); a.mode = 1; missingClr = true; } break;
          case DrawAttr::MtxIdx: a.bytes = 4; if (group) a.mode = 3; else if (V.hasPmIdx) { a.src = V.pmIdx.data(); a.step = 1; a.mode = 2; } else a.mode = 1; break;
          case DrawAttr::Tex: a.bytes = 8; if (V.hasTex[sa.index]) { a.src = reinterpret_cast<const uint8_t*>(V.tex[sa.index].data()); a.step = 8; } else a.mode = 1; break;
        }
      }
      uint8_t* row = packedVertices + size_t(vertexBase) * stride;
      for (uint32_t i = 0; i < count; i++, row += stride) {
        for (size_t j = 0; j < na; j++) {
          const VmAttr& a = va[j]; uint8_t* dst = row + a.off;
          switch (a.mode) {
            case 0: { const uint8_t* s0 = a.src + size_t(i) * a.step;
              if (a.bytes == 12) std::memcpy(dst, s0, 12); else if (a.bytes == 8) std::memcpy(dst, s0, 8); else std::memcpy(dst, s0, 4); break; }
            case 1: if (a.bytes == 12) std::memcpy(dst, a.k, 12); else if (a.bytes == 8) std::memcpy(dst, a.k, 8); else std::memcpy(dst, a.k, 4); break;
            case 2: { const uint8_t b[4] = {a.src[i], 0, 0, 0}; std::memcpy(dst, b, 4); break; }
            default: { const unsigned k = V.hasPmIdx ? V.pmIdx[i] / 3 : 0; const uint8_t b[4] = {uint8_t(3 * group->slots[member][k]), 0, 0, 0}; std::memcpy(dst, b, 4); break; }
          }
        }
      }
      vertexBase += count;
    }
  } else
  for (const SubmitAttr& sa : P.layout) {
   uint32_t vertexBase=0;
   for(size_t member=0;member<(group?group->count:1);++member) {
    const GxVertices& V=group?group->draws[member]->v:d.v;
    const uint32_t count=V.count;
    uint8_t* dst = packedVertices + sa.offset + size_t(vertexBase)*stride;
    switch (sa.kind) {
      case DrawAttr::Pos:
        if (V.hasPos) { const float* src = V.pos.data(); for (uint32_t i = 0; i < count; i++, dst += stride, src += 3) std::memcpy(dst, src, 12); }
        else for (uint32_t i = 0; i < count; i++, dst += stride) std::memset(dst, 0, 12);
        break;
      case DrawAttr::Nrm:
        if (V.hasNrm) { const float* src = V.nrm.data(); for (uint32_t i = 0; i < count; i++, dst += stride, src += 3) std::memcpy(dst, src, 12); }
        else { const float f[3] = {0, 0, 1}; missingNrm = true; for (uint32_t i = 0; i < count; i++, dst += stride) std::memcpy(dst, f, 12); }
        break;
      case DrawAttr::Clr0:
        if (V.hasClr[0]) { const uint8_t* src = V.clr[0].data(); for (uint32_t i = 0; i < count; i++, dst += stride, src += 4) std::memcpy(dst, src, 4); }
        else { missingClr = true; for (uint32_t i = 0; i < count; i++, dst += stride) std::memset(dst, 255, 4); }
        break;
      case DrawAttr::MtxIdx:
        if(group)for(uint32_t i=0;i<count;i++,dst+=stride){
          const unsigned k=V.hasPmIdx?V.pmIdx[i]/3:0;
          const uint8_t b[4]={uint8_t(3*group->slots[member][k]),0,0,0};std::memcpy(dst,b,4);
        }
        else if (V.hasPmIdx) { const uint8_t* src = V.pmIdx.data(); for (uint32_t i = 0; i < count; i++, dst += stride) { const uint8_t b[4] = {src[i], 0, 0, 0}; std::memcpy(dst, b, 4); } }
        else for (uint32_t i = 0; i < count; i++, dst += stride) std::memset(dst, 0, 4);
        break;
      case DrawAttr::Tex:
        if (V.hasTex[sa.index]) { const float* src = V.tex[sa.index].data(); for (uint32_t i = 0; i < count; i++, dst += stride, src += 2) std::memcpy(dst, src, 8); }
        else for (uint32_t i = 0; i < count; i++, dst += stride) std::memset(dst, 0, 8);
        break;
    }
    vertexBase+=count;
   }
  }
  if(!P.externalIndices) P.indices.resize(indexCount);
  uint16_t* packedIndices=P.externalIndices?P.externalIndices:P.indices.data();
  // (Claude v77) the largest packed index, taken while converting: c3dplan's planGeometry then skips its own pass over the indices
  // (indicesChecked); it refuses exactly when this maximum is not below vertexCount, as its loop did.
  uint32_t maxIndex=0;
  if(group){
    uint32_t base=0;size_t at=0;
    for(size_t m=0;m<group->count;m++){
      for(auto i:group->draws[m]->tris){const uint16_t v=uint16_t(base+i);packedIndices[at++]=v;if(v>maxIndex)maxIndex=v;}
      base+=group->draws[m]->v.count;
    }
  }else for (size_t i = 0; i < d.tris.size(); i++) { const uint16_t v = uint16_t(d.tris[i]); packedIndices[i] = v; if (v > maxIndex) maxIndex = v; }
  P.indicesChecked = maxIndex < P.vertexCount;
  }
  P.packMissingNrm = missingNrm; P.packMissingClr = missingClr;
  if (missingNrm) P.notes.push_back("lighting reads normals but the vertex descriptor has none: (0,0,1) is used");
  if (missingClr) P.notes.push_back("the shader reads vertex colour but the vertex descriptor has none: white is used");
  const uint64_t pt1 = t2pProfTicks();
  if (t2pProfEnabled()) g_t2pProf[9] += pt1 - pt0;
  struct ProfRest { uint64_t t; ~ProfRest() { if (t2pProfEnabled()) g_t2pProf[10] += t2pProfTicks() - t; } } profRest{pt1};

  // ---------------------------------------------------------------- uniforms

  // ---------------------------------------------------------------- texenv words with runtime constants resolved
  {
    Inputs in;
    for (int i = 0; i < 4; i++) in.kcolor[i] = S.kcolor[i];
    for (int i = 0; i < 3; i++) in.reg[i] = S.tevReg[i + 1];
    for (int i = 0; i < 4; i++)
      for (int c = 0; c < 4; c++)
        if (S.tevReg[i].v[c] < 0.0f || S.tevReg[i].v[c] > 1.0f) {
          P.notes.push_back("initial TEV register " + num(i) + " is outside 0..1 (signed 11 bit): the constant is clamped");
          i = 4; break;
        }
    float key[7][4];
    for (int i = 0; i < 4; i++) std::memcpy(key[i], in.kcolor[i].v, 16);
    for (int i = 0; i < 3; i++) std::memcpy(key[4 + i], in.reg[i].v, 16);
    PicaDraw::TexEnvMemo& m = P.pica().texenvMemo;
    if (m.valid && std::memcmp(m.in, key, sizeof key) == 0) {
      P.texenv.assign(m.words.begin(), m.words.end());
      P.texenvUpdateBuffer = m.updateBuffer;
    } else {
      for (const PStage& st : P.pica().tev.prog.stages) P.texenv.push_back(encodeStage(st, resolveConst(st, in)));
      P.texenvUpdateBuffer = encodeBufferUpdate(P.pica().tev.prog);
      m.words.assign(P.texenv.begin(), P.texenv.end()); m.updateBuffer = P.texenvUpdateBuffer; std::memcpy(m.in, key, sizeof key); m.valid = true;
    }
    for (int i = 0; i < g_benchRepeat[4]; i++) {
      static std::vector<HwStage> sink; sink.clear();
      for (const PStage& st : P.pica().tev.prog.stages) sink.push_back(encodeStage(st, resolveConst(st, in)));
      volatile uint32_t v = encodeBufferUpdate(P.pica().tev.prog); (void)v;
    }
  }

  // ---------------------------------------------------------------- texture units
  for (int u = 0; u < P.pica().numUnits; u++) {
    SubmitUnit& U = P.units[u];
    U.texmap = P.pica().unitTexmap[u]; U.texcoord = P.pica().unitTexcoord[u];
    U.scratch = U.texmap == kScratchTexmap;
    if (U.texmap >= 0 && U.texmap < 8) {
      U.tex = S.tex[U.texmap];
      if (!U.tex.valid) P.notes.push_back("texmap " + num(U.texmap) + " is sampled but its image registers were never written");
    }
    U.needAlpha = P.pica().unitNeedAlpha[u];
    if (U.tex.valid) {
      U.scaleS = axisScale(planAxis(U.tex.width, U.tex.wrapS));
      U.scaleT = axisScale(planAxis(U.tex.height, U.tex.wrapT));
    }
  }

  // ---------------------------------------------------------------- uniforms (the texture scales depend on the textures bound)
  if(preparedUniforms){P.packedUniforms=std::move(preparedUniforms);P.usesPackedUniforms=true;P.uniforms.clear();P.ok=true;return;}
  if (packed) {
    if (!P.packedUniforms || !P.packedUniforms.unique()) P.packedUniforms=std::make_shared<PackedUniforms>();
    float scale[3][2];std::memcpy(scale,S.uniformView().texScale,sizeof scale);
    for(int u=0;u<P.pica().numUnits;u++){scale[u][0]=P.units[u].scaleS;scale[u][1]=P.units[u].scaleT;}
    packUniformsPacked(*P.packedUniforms,P.pica().vtx,P.pica().vs,S.uniformView(),P.frame.proj,scale);
    for(int i=0;i<g_benchRepeat[2];i++)packUniformsPacked(*P.packedUniforms,P.pica().vtx,P.pica().vs,S.uniformView(),P.frame.proj,scale);
    P.usesPackedUniforms=true;P.uniforms.clear();P.ok=true;return;
  }
  VtxUniforms uni = S.vertexUniforms();
  std::copy(&P.frame.proj[0][0], &P.frame.proj[0][0] + 16, &uni.proj[0][0]);
  for (int u = 0; u < P.pica().numUnits; u++) { uni.texScale[u][0] = P.units[u].scaleS; uni.texScale[u][1] = P.units[u].scaleT; }
  packUniformsInto(P.uniforms, P.pica().vtx, P.pica().vs, uni);
  P.ok = true;
}

}  // namespace

DrawPacket buildDraw(const GxState& S, const GxDraw& d, const Options& opt, const FrameConfig& frameCfg, DrawCompileCache* cache) {
  DrawPacket P;
  buildDrawInto(P, S, d, opt, frameCfg, cache, true);
  return P;
}

PackedGeometryCache::Entry* PackedGeometryCache::find(uint64_t key, uint64_t geoId, uint32_t stride, const std::vector<uint32_t>& sig) {
  auto it = map_.find(key);
  if (it == map_.end()) return nullptr;
  Entry& e = it->second;
  if (e.geoId != geoId || e.stride != stride || e.sig != sig) return nullptr;
  e.frame = frame_;
  return &e;
}

PackedGeometryCache::Entry* PackedGeometryCache::insert(uint64_t key) {
  if (bytes_ > budget_) return nullptr;
  Entry& e = map_[key];
  bytes_ -= std::min(bytes_, e.vertices.capacity() + e.indices.capacity() * 2);
  e.frame = frame_;
  return &e;
}

void PackedGeometryCache::endFrame() {
  size_t b = 0;
  for (auto it = map_.begin(); it != map_.end();) {
    if (it->second.frame != frame_) it = map_.erase(it);
    else { b += it->second.vertices.capacity() + it->second.indices.capacity() * 2; ++it; }
  }
  bytes_ = b;
  frame_++;
}

void releasePacked(DrawPacket& P, PackedGeometryCache& cache, bool planOk) {
  if (P.packedFrom) {
    PackedGeometryCache::Entry* e = P.packedFrom;
    std::swap(P.vertices, e->vertices); std::swap(P.indices, e->indices);
    if (planOk) e->indicesChecked = true;
  } else if (P.packKey && P.ok) {
    if (PackedGeometryCache::Entry* e = cache.insert(P.packKey)) {
      e->geoId = P.packGeoId; e->stride = P.stride; e->sig = P.packSig;
      e->missingNrm = P.packMissingNrm; e->missingClr = P.packMissingClr; e->indicesChecked = planOk;
      std::swap(P.vertices, e->vertices); std::swap(P.indices, e->indices);
      P.vertices.clear(); P.indices.clear();
    }
  }
  P.packedFrom = nullptr; P.packKey = 0;
}

namespace {
// P back to a default packet, its containers emptied but keeping their capacity (the uniform map keeps its keys: packUniformsInto rewrites every one it uses)
void recycle(DrawPacket& P) {
  auto packed = std::move(P.packedUniforms);
  UniformMap uniforms = std::move(P.uniforms);
  std::vector<SubmitAttr> layout = std::move(P.layout);
  if (P.packedFrom) {   // buffers swapped in from a PackedGeometryCache and not released: they go back, never cleared here
    std::swap(P.vertices, P.packedFrom->vertices); std::swap(P.indices, P.packedFrom->indices);
    P.packedFrom = nullptr;
  }
  std::vector<uint8_t> vertices = std::move(P.vertices);
  std::vector<uint16_t> indices = std::move(P.indices);
  std::vector<HwStage> texenv = std::move(P.texenv);
  std::vector<std::string> notes = std::move(P.notes);
  std::vector<uint32_t> packSig = std::move(P.packSig);
  P = DrawPacket();
  layout.clear(); vertices.clear(); indices.clear(); texenv.clear(); notes.clear(); packSig.clear();
  P.packSig = std::move(packSig);
  P.packedUniforms = std::move(packed);
  P.uniforms = std::move(uniforms);
  P.layout = std::move(layout);
  P.vertices = std::move(vertices);
  P.indices = std::move(indices);
  P.texenv = std::move(texenv);
  P.notes = std::move(notes);
}
}  // namespace

void buildDrawInto(DrawPacket& P, const GxState& S, const GxDraw& d, const Options& opt, const FrameConfig& frameCfg, DrawCompileCache* cache, bool notes,
                   PackedGeometryCache* packCache, uint64_t geoId, const PacketGeometryOutput* output, const matrixbatch::Group* group, std::shared_ptr<const PicaDraw> compiled, std::shared_ptr<PackedUniforms> preparedUniforms) {
  recycle(P);
  if(group && (packCache || geoId || group->count<2 || group->vertices>matrixbatch::Group::MaxVertices)) {
    P.uniforms.clear();P.notes.push_back("invalid matrix group");return;
  }
  if (notes) P.notes = S.notes;
  P.viewport = S.viewport; P.scissor = S.scissor; P.cullMode = S.cullMode;
  P.colorUpdate = S.colorUpdate; P.alphaUpdate = S.alphaUpdate; P.dither = S.dither;
  P.dstAlphaEnable = S.dstAlphaEnable; P.dstAlpha = S.dstAlpha; P.pixelFmt = S.pixelFmt; P.zCompBeforeTex = S.zCompBeforeTex;

  // a packet that is not filled has no uniforms (the recycled map would still hold the previous draw's)
  if (d.unsupportedPrim) { P.uniforms.clear(); P.notes.push_back("lines / points are not assembled: draw skipped"); return; }
  if (d.tris.empty()) { P.uniforms.clear(); P.notes.push_back("draw has no triangles"); return; }
  if (d.v.count > 65536) { P.uniforms.clear(); P.notes.push_back("draw has " + num(int(d.v.count)) + " vertices: more than a 16 bit index list addresses (65536)"); return; }

  const uint64_t pt0 = t2pProfTicks();
  P.setPica(compiled ? std::move(compiled) : cache ? cache->get(S.draw, opt) : std::make_shared<PicaDraw>(compileDraw(S.draw, opt)));
  if (cache) for (int i = 0; i < g_benchRepeat[5]; i++) { volatile bool v = bool(cache->get(S.draw, opt)); (void)v; }
  if (t2pProfEnabled()) g_t2pProf[8] += t2pProfTicks() - pt0;
  if (notes) for (const std::string& n : P.pica().notes) P.notes.push_back(n);
  if (P.pica().overall == Fidelity::Unsupported) {
    for (const std::string& b : P.pica().blockers) P.notes.push_back("unsupported: " + b);
    P.uniforms.clear();
    return;
  }
  if(preparedUniforms && P.pica().pass2){P.notes.push_back("prepared rows cannot serve a multipass draw");return;}
  if(group && P.pica().pass2){P.uniforms.clear();P.notes.push_back("matrix group cannot use multipass");return;}
  fillPacket(P, S, d, frameCfg, notes, opt.packedUniforms, packCache, geoId, P.pica().pass2 ? nullptr : output,group,std::move(preparedUniforms));
  if (!P.ok) P.uniforms.clear();   // fillPacket writes the uniforms last, together with ok
  if (P.ok && P.pica().pass2) {   // the second pass of a two-pass draw: the same geometry through its own compiled draw
    P.second = std::make_shared<DrawPacket>();
    P.second->setPica(P.pica().pass2);
    fillPacket(*P.second, S, d, frameCfg, notes, opt.packedUniforms);
    if (!P.second->ok) {
      P.ok = false;
      P.notes.push_back("second pass of a two-pass draw: packet refused");
      for (const std::string& n : P.second->notes) P.notes.push_back(n);
    }
  }
}

}  // namespace t2p
