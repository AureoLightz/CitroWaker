#include "gxbatch.h"

#include <algorithm>
#include <cstring>

namespace t2p {
namespace {
template<class V> size_t compactTail(V& v){
 const size_t used=v.size()*sizeof(typename V::value_type),old=v.capacity()*sizeof(typename V::value_type);
 if(old-used<1024 || old-used<used/2)return 0;
 // shrink_to_fit leaves a valid original vector if allocation fails; no peak
 // whole-frame copy, and capacity reduction is measured rather than assumed.
 v.shrink_to_fit();return old-v.capacity()*sizeof(typename V::value_type);
}
}
size_t compactDrawStorage(GxDraw& d){
 size_t n=compactTail(d.tris);auto& v=d.v;n+=compactTail(v.pos)+compactTail(v.nrm)+compactTail(v.pmIdx);
 for(int c=0;c<2;++c)n+=compactTail(v.clr[c]);
 for(int t=0;t<8;++t)n+=compactTail(v.tex[t])+compactTail(v.texMtxIdx[t]);return n;
}


static bool sameVcd(const GxVcd& a, const GxVcd& b) {
  return a.pmIdx == b.pmIdx && a.pos == b.pos && a.nrm == b.nrm && std::memcmp(a.texMtxIdx, b.texMtxIdx, sizeof a.texMtxIdx) == 0 &&
         std::memcmp(a.clr, b.clr, sizeof a.clr) == 0 && std::memcmp(a.tex, b.tex, sizeof a.tex) == 0;
}

static bool sameAttrs(const GxVertices& a, const GxVertices& b) {
  if (a.hasPos != b.hasPos || a.hasNrm != b.hasNrm || a.hasPmIdx != b.hasPmIdx) return false;
  for (int i = 0; i < 2; i++) if (a.hasClr[i] != b.hasClr[i]) return false;
  for (int i = 0; i < 8; i++) if (a.hasTex[i] != b.hasTex[i] || a.hasTexMtxIdx[i] != b.hasTexMtxIdx[i]) return false;
  return true;
}

bool canAppend(const GxDraw& into, const GxDraw& d, uint32_t maxVertices) {
  if (into.unsupportedPrim || d.unsupportedPrim || gxTriCount(into) == 0 || gxTriCount(d) == 0) return false;
  if (uint64_t(into.v.count) + d.v.count > maxVertices) return false;
  return sameVcd(into.vcd, d.vcd) && sameAttrs(into.v, d.v);
}

bool canAppendHeader(const GxDraw& into, const GxDraw& h, uint32_t maxVertices) {
  if (into.unsupportedPrim || h.unsupportedPrim || gxTriCount(into) == 0 || h.vertexCount < 3) return false;
  if (uint64_t(into.v.count) + h.vertexCount > maxVertices) return false;
  return sameVcd(into.vcd, h.vcd);
}

int maxPosMtxFrom(const GxDraw& d, uint32_t first) {
  int m = -1;
  for (size_t i = first; i < d.v.pmIdx.size(); i++) m = std::max<int>(m, d.v.pmIdx[i] / 3);
  return m;
}

template <class V>
static void cat(V& a, const V& b) { a.insert(a.end(), b.begin(), b.end()); }

void appendDraw(GxDraw& into, const GxDraw& d) {
  const uint32_t base = into.v.count;
  GxVertices& A = into.v;
  const GxVertices& B = d.v;
  cat(A.pos, B.pos);
  cat(A.nrm, B.nrm);
  for (int i = 0; i < 2; i++) cat(A.clr[i], B.clr[i]);
  for (int i = 0; i < 8; i++) { cat(A.tex[i], B.tex[i]); cat(A.texMtxIdx[i], B.texMtxIdx[i]); }
  cat(A.pmIdx, B.pmIdx);
  A.count += B.count;
  into.tris.reserve(into.tris.size() + d.tris.size());
  for (uint32_t t : d.tris) into.tris.push_back(base + t);
  into.vertexCount += d.vertexCount;
  into.commandCount += d.commandCount;
  into.prim = GXP_TRIANGLES;
}

int maxPosMtx(const GxDraw& d) {
  int m = -1;
  for (uint8_t i : d.v.pmIdx) m = std::max<int>(m, i / 3);
  return m;
}

}  // namespace t2p
