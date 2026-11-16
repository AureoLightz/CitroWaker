#pragma once
// Adjacent perspective draws may share one palette without changing vertex math.
// This helper never reorders draws or mutates published uniform blocks. The caller
// must enforce event barriers, shader support, register limits and debug hooks.
#include <array>
#include <cstring>
#include "drawcache.h"
#include "gxstate.h"
#include "gxretain.h"
namespace t2p { namespace matrixbatch {
// (Claude v77 profiling) set by the caller around sampled attempts; [0..MB_N) refusal counts, ticks: cheap, nonPose, heavy
struct Prof { bool on=false; uint32_t why[16]={}; uint64_t ticks[4]={}; uint32_t calls[4]={}; uint64_t np[6]={}; uint64_t (*clock)()=nullptr; };
inline Prof& prof() { static thread_local Prof p; return p; }
// (Claude v77) bit equality of float arrays as inline 32-bit word compares (was a memcmp call per field: ~40 calls per nonPose on the ARM11).
// Same result as memcmp(...)==0: equal iff every word is equal. Long arrays OR 4 word differences per step.
static inline bool fb(const float* a, const float* b, size_t n) {
  const uint32_t* x = reinterpret_cast<const uint32_t*>(a); const uint32_t* y = reinterpret_cast<const uint32_t*>(b);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) if ((x[i] ^ y[i]) | (x[i+1] ^ y[i+1]) | (x[i+2] ^ y[i+2]) | (x[i+3] ^ y[i+3])) return false;
  for (; i < n; i++) if (x[i] != y[i]) return false;
  return true;
}
static bool sameTex(const GxTexBinding& a, const GxTexBinding& b) {
  return a.valid == b.valid && a.addr == b.addr && a.width == b.width && a.height == b.height && a.fmt == b.fmt && a.wrapS == b.wrapS && a.wrapT == b.wrapT &&
         a.magLinear == b.magLinear && a.minLinear == b.minLinear && a.mipMode == b.mipMode && fb(&a.lodBias, &b.lodBias, 1) && fb(&a.minLod, &b.minLod, 1) &&
         fb(&a.maxLod, &b.maxLod, 1) && a.edgeLod == b.edgeLod && a.biasClamp == b.biasClamp && a.aniso == b.aniso && a.tlutOffset == b.tlutOffset && a.tlutFmt == b.tlutFmt;
}
static bool sameLight(const VtxLight& a, const VtxLight& b) {
  static_assert(sizeof(VtxLight)==16*sizeof(float),"light has no padding");
  return fb(reinterpret_cast<const float*>(&a),reinterpret_cast<const float*>(&b),16);
}
enum Why { MB_OK, MB_BARRIER, MB_ORTHO, MB_UNSUPPORTED, MB_DRAWSTATE, MB_PIXEL, MB_TEXTURE, MB_VIEW, MB_UNIFORM, MB_ATTRS, MB_PMIDX, MB_PALETTE, MB_VERTS, MB_VARIANT, MB_N };
static const char* kWhy[MB_N] = {"ok", "barrier", "ortho", "unsupported/empty", "drawstate", "pixel/const", "texture", "viewport/scissor/proj", "uniform", "attrs", "pmidx", "palette>10", "vertices", "variant"};

// a member's own shape: everything that may differ is the pose; the rest must be equal
template<class Member>
static Why nonPose(const Member& A, const Member& B, const PicaDraw& pa, const Options& opt, bool usedOnly, bool keyProven = false) {
  const GxState &a = A.st, &b = B.st;
  Prof& PP=prof(); uint64_t q0=PP.on?PP.clock():0;
  auto mark=[&](int k){ if(PP.on){uint64_t t=PP.clock();PP.np[k]+=t-q0;q0=t;} };
  // (Claude v87) keyProven: the recorder proves (dirtyIn along the chain) that only TEV registers, viewport/scissor, matrix memory or lights were written,
  // so every GxDrawState field equals member 0's except numPosMtx (ignored here) and texmapScaled (compared); the same proof as the renderer's key memo
  if (keyProven ? a.draw.texmapScaled != b.draw.texmapScaled : !sameDrawKeyIgnoringPoseCount(a.draw,b.draw)) return MB_DRAWSTATE;
  mark(0);
  for (int i = 0; i < 4; i++) if (!fb(a.kcolor[i].v, b.kcolor[i].v, 4) || !fb(a.tevReg[i].v, b.tevReg[i].v, 4)) return MB_PIXEL;
  if (a.cullMode != b.cullMode || a.colorUpdate != b.colorUpdate || a.alphaUpdate != b.alphaUpdate || a.dither != b.dither || a.dstAlphaEnable != b.dstAlphaEnable ||
      a.dstAlpha != b.dstAlpha || a.pixelFmt != b.pixelFmt || a.zFmt16 != b.zFmt16 || a.zCompBeforeTex != b.zCompBeforeTex || a.numTexGens != b.numTexGens ||
      a.numChans != b.numChans || a.numTevStages != b.numTevStages || a.numIndStages != b.numIndStages)
    return MB_PIXEL;
  mark(1);
  if (!usedOnly) { for (int i = 0; i < 8; i++) if (!sameTex(a.tex[i], b.tex[i])) return MB_TEXTURE; }
  else   // (Claude v77) fillPacket reads S.tex only for the texmaps of the shader's units (submit.cpp "texture units"); the texmap-scaled key bits come from member 0
    for (int u = 0; u < pa.numUnits && u < 3; u++) { const int t = pa.unitTexmap[u]; if (t >= 0 && t < 8 && !sameTex(a.tex[t], b.tex[t])) return MB_TEXTURE; }
  mark(2);
  if (!fb(&a.viewport.x, &b.viewport.x, 1) || !fb(&a.viewport.y, &b.viewport.y, 1) || !fb(&a.viewport.width, &b.viewport.width, 1) || !fb(&a.viewport.height, &b.viewport.height, 1) ||
      !fb(&a.viewport.nearZ, &b.viewport.nearZ, 1) || !fb(&a.viewport.farZ, &b.viewport.farZ, 1) ||
      a.scissor.x != b.scissor.x || a.scissor.y != b.scissor.y || a.scissor.width != b.scissor.width || a.scissor.height != b.scissor.height ||
      a.proj.ortho != b.proj.ortho || !fb(a.proj.p, b.proj.p, 6) || !fb(&a.proj.nearZ, &b.proj.nearZ, 1) || !fb(&a.proj.farZ, &b.proj.farZ, 1) || !fb(&a.proj.m[0][0], &b.proj.m[0][0], 16))
    return MB_VIEW;
  mark(3);
  const auto u=a.uniformView(),v=b.uniformView();
  if (!fb(&u.proj[0][0], &v.proj[0][0], 16) || !fb(u.matColor, v.matColor, 4) || !fb(u.ambColor, v.ambColor, 4) || !fb(&u.texScale[0][0], &v.texScale[0][0], 6)) return MB_UNIFORM;
  if (!usedOnly) {
    static_assert(sizeof(VtxLight)==16*sizeof(float),"light has no padding");
    static_assert(sizeof u.light%4==0,"light array is whole words");
    if(!fb(reinterpret_cast<const float*>(u.light),reinterpret_cast<const float*>(v.light),sizeof u.light/4))return MB_UNIFORM;
    if (!fb(&u.texMtx[0][0][0], &v.texMtx[0][0][0], 120)) return MB_UNIFORM;
  } else {
    for (int l = 0; l < 8; l++) if (pa.vs.lightSlot[l] >= 0 && !sameLight(u.light[l], v.light[l])) return MB_UNIFORM;
    for (size_t i = 0; i < pa.vtx.texgen.size() && i < 3; i++)
      if (pa.vs.texMtxSlot[i] >= 0) { const int k = (pa.vtx.texgen[i].mtx - 30) / 3; if (!fb(&u.texMtx[k][0][0], &v.texMtx[k][0][0], 12)) return MB_UNIFORM; }
  }
  mark(4);
  // decoded attribute presence: the packed layout and the missing-attribute defaults must be the same
  const GxVertices &x = A.d.v, &y = B.d.v;
  if (x.hasPos != y.hasPos || x.hasNrm != y.hasNrm || x.hasClr[0] != y.hasClr[0] || x.hasClr[1] != y.hasClr[1]) return MB_ATTRS;
  for (int t = 0; t < 8; t++) if (x.hasTex[t] != y.hasTex[t] || x.hasTexMtxIdx[t] || y.hasTexMtxIdx[t]) return MB_ATTRS;
  return MB_OK;
}

// (Claude v78) nonPose when the recorder proves (RecordedEvent3ds::dirtyIn) that between member 0 and B only matrix memory (XF 0..0x5FF) changed:
// every section decodeStateIncremental took from the previous state is equal by construction (draw key, pixel, textures, view, projection, colours,
// lights), so of the GxState only the texture matrices the shader reads can differ (positions / normals are the pose). The attribute check is the same.
template<class Member>
static Why nonPoseXfOnly(const Member& A, const Member& B, const PicaDraw& pa) {
  const auto u=A.st.uniformView(),v=B.st.uniformView();
  for (size_t i = 0; i < pa.vtx.texgen.size() && i < 3; i++)
    if (pa.vs.texMtxSlot[i] >= 0) { const int k = (pa.vtx.texgen[i].mtx - 30) / 3; if (!fb(&u.texMtx[k][0][0], &v.texMtx[k][0][0], 12)) return MB_UNIFORM; }
  const GxVertices &x = A.d.v, &y = B.d.v;
  if (x.hasPos != y.hasPos || x.hasNrm != y.hasNrm || x.hasClr[0] != y.hasClr[0] || x.hasClr[1] != y.hasClr[1]) return MB_ATTRS;
  for (int t = 0; t < 8; t++) if (x.hasTex[t] != y.hasTex[t] || x.hasTexMtxIdx[t] || y.hasTexMtxIdx[t]) return MB_ATTRS;
  return MB_OK;
}

// ---------------------------------------------------------------- the pose of each vertex: (position rows, normal rows) by bits
struct Pose { float p[3][4]; float n[3][4]; };
static bool samePose(const Pose& a, const Pose& b, bool withNrm) { return fb(&a.p[0][0], &b.p[0][0], 12) && (!withNrm || fb(&a.n[0][0], &b.n[0][0], 12)); }
// the matrix number a vertex of this draw selects as the CURRENT renderer computes it (decodeState folds curPosMtx into slot 0 without PNMTXIDX)
template<class Member>
static bool vertexMatrix(const Member& e, uint32_t i, int& k) {
  if (!e.d.v.hasPmIdx) { k = 0; return true; }
  const uint8_t r = e.d.v.pmIdx[i];
  if (r % 3 || r / 3 > 9) return false;
  k = r / 3; return true;
}
template<class Member>
static Pose poseOf(const Member& e, int k) { Pose q; const auto U=e.st.uniformView(); std::memcpy(q.p, U.pos[k], sizeof q.p); std::memcpy(q.n, U.nrm[k], sizeof q.n); return q; }


struct MemberView { const GxState& st; const GxDraw& d; };
class Group {
 public:
  static constexpr size_t MaxMembers=64;
  static constexpr uint32_t MaxVertices=4096;
  std::array<const GxState*,MaxMembers> states{};
  std::array<const GxDraw*,MaxMembers> draws{};
  std::array<std::array<int8_t,10>,MaxMembers> slots{};
  std::array<GeometryPmMemo*,MaxMembers> memos{};   // (Claude v122) a member's retained-geometry memo (null: not retained, scanned every time)
  std::array<Pose,10> palette{};
  size_t count=0, paletteSize=0;
  uint32_t vertices=0;
  bool withNormal=false;
  void clear() { count=paletteSize=0;vertices=0;withNormal=false;firstDeferred=false;chainDirty=0; }
  // (Claude v69) The O(vertices + triangles) part of a member's validation (index range, pose palette) is deferred for the FIRST member until a
  // second member has passed the cheap checks and nonPose: most draws never get a second member, and a one-member group is never used.
  // Acceptance is unchanged (every check is a pure function of the members; the palette is still filled first member first); only the Why
  // reported for a refused append can differ.
  bool firstDeferred=false;
  Why cheapChecks(const GxState& st,const GxDraw& d,const PicaDraw& shader) const {
    if(st.proj.ortho) return MB_ORTHO;
    if(d.unsupportedPrim || !d.v.count || !d.v.hasPos || d.tris.empty() || d.tris.size()%3 || shader.overall==Fidelity::Unsupported || shader.pass2 || !shader.vs.ok) return MB_UNSUPPORTED;
    if(count==MaxMembers || d.v.count>MaxVertices-vertices) return MB_VERTS;
    const auto& v=d.v;
    if(v.pos.size()<size_t(v.count)*3 || (v.hasNrm && v.nrm.size()<size_t(v.count)*3) ||
       (v.hasPmIdx && v.pmIdx.size()<v.count) || (!v.hasPmIdx && (st.curPosMtx<0 || st.curPosMtx>=10))) return MB_UNSUPPORTED;
    for(int c=0;c<2;c++) if(v.hasClr[c] && v.clr[c].size()<size_t(v.count)*4) return MB_UNSUPPORTED;
    for(int t=0;t<8;t++) if(v.hasTexMtxIdx[t] || (v.hasTex[t] && v.tex[t].size()<size_t(v.count)*2)) return MB_ATTRS;
    return MB_OK;
  }
  // index range + pose palette of member `at` (already in states/draws); commits its slot map and the palette only when every pose fits
  // (Claude v122) the geometry-only part of heavyChecks: the index range and the distinct matrix slots in the order the vertex scan meets them
  static void fillMemo(const GxDraw& d,GeometryPmMemo& M) {
    const auto& v=d.v;M.n=0;
    for(auto i:d.tris) if(i>=v.count) { M.state=-1;M.why=MB_UNSUPPORTED;return; }
    if(!v.hasPmIdx) { M.k[0]=0;M.n=1;M.state=1;return; }
    bool seen[10]={};int prevIdx=-1;
    for(uint32_t i=0;i<v.count;i++) {
      const int r=v.pmIdx[i];if(r==prevIdx)continue;prevIdx=r;
      if(r%3 || r/3>9) { M.state=-1;M.why=MB_PMIDX;return; }
      const int k=r/3;if(!seen[k]) { seen[k]=true;M.k[M.n++]=int8_t(k); }
    }
    M.state=1;
  }
  Why heavyChecks(size_t at,const PicaDraw& shader) {
    const GxState& st=*states[at];const GxDraw& d=*draws[at];const auto& v=d.v;
    MemberView m{st,d};
    std::array<Pose,10> pending;size_t n=paletteSize;
    std::array<int8_t,10> map;map.fill(-1);
    const bool normal=shader.vs.usesNormal;
    if(GeometryPmMemo* M=memos[at]) {   // (Claude v122) same palette and map as the scan below: it meets the same slots in the same order
      if(!M->state) fillMemo(d,*M);
      if(M->state<0) return Why(M->why);
      for(int j0=0;j0<M->n;j0++) {
        const int k=M->k[j0];
        Pose q=poseOf(m,k);size_t j=0;
        while(j<n && !samePose(j<paletteSize?palette[j]:pending[j-paletteSize],q,normal)) ++j;
        if(j==n) { if(n==10) return MB_PALETTE;pending[n++-paletteSize]=q; }
        map[k]=int8_t(j);
      }
      if(n>paletteSize)std::memcpy(palette.data()+paletteSize,pending.data(),(n-paletteSize)*sizeof(Pose));
      paletteSize=n;slots[at]=map;
      return MB_OK;
    }
    for(auto i:d.tris) if(i>=v.count) return MB_UNSUPPORTED;
    int prevIdx=-1;   // (Claude v78) a vertex repeating the previous vertex's index byte: same matrix, already mapped (or the loop has returned)
    for(uint32_t i=0;i<(v.hasPmIdx?v.count:1u);i++) {
      if(v.hasPmIdx){const int r=v.pmIdx[i];if(r==prevIdx)continue;prevIdx=r;}
      int k=0;if(!vertexMatrix(m,i,k)) return MB_PMIDX;
      if(map[k]>=0) continue;
      Pose q=poseOf(m,k);size_t j=0;
      while(j<n && !samePose(j<paletteSize?palette[j]:pending[j-paletteSize],q,normal)) ++j;
      if(j==n) { if(n==10) return MB_PALETTE;pending[n++-paletteSize]=q; }
      map[k]=int8_t(j);
    }
    if(n>paletteSize)std::memcpy(palette.data()+paletteSize,pending.data(),(n-paletteSize)*sizeof(Pose));
    paletteSize=n;slots[at]=map;
    return MB_OK;
  }
  // (Claude v78) chainDirty: OR of the members' dirtyIn after member 0 (dirtyIn: groups changed since the previous event's state, all bits when
  // unknown). Only XF matrix memory changed along the chain: nonPoseXfOnly. Appended members are consecutive events (the caller stops at any other).
  uint32_t chainDirty=0;
  Why append(const GxState& st,const GxDraw& d,const PicaDraw& shader,const Options& opt,uint32_t dirtyIn=0xFFFFFFFFu,GeometryPmMemo* memo=nullptr) {
    Prof& P=prof();
    if(!P.on) return appendImpl(st,d,shader,opt,dirtyIn,memo);
    Why w=appendImpl(st,d,shader,opt,dirtyIn,memo);P.why[w]++;return w;
  }
  Why appendImpl(const GxState& st,const GxDraw& d,const PicaDraw& shader,const Options& opt,uint32_t dirtyIn,GeometryPmMemo* memo) {
    Prof& P=prof();uint64_t t0=P.on?P.clock():0;
    Why w=cheapChecks(st,d,shader);
    if(P.on){uint64_t t=P.clock();P.ticks[0]+=t-t0;P.calls[0]++;t0=t;}
    if(w!=MB_OK) return w;
    MemberView m{st,d};
    if(count) {
      if(shader.vs.usesNormal!=withNormal) return MB_DRAWSTATE;
      // (Claude v77) usedOnly: the grouped draw uploads member 0's uniforms through packUniformsPacked / packUniforms (vtx.cpp), which read only the lights
      // with a lightSlot and the texture matrices with a texMtxSlot (texgens 0..2) of this shader; frame.cpp reads proj (compared) and the pose (replaced).
      // Lights / texture matrices the shader never reads cannot change the output, so only the used ones must be equal.
      const uint32_t chain=chainDirty|dirtyIn;
      constexpr uint32_t kKeySafe=kStateDirtyTevReg|kStateDirtyView|kStateDirtyXfMtx|kStateDirtyLight;
      w=(chain&~uint32_t(kStateDirtyXfMtx))==0 ? nonPoseXfOnly(MemberView{*states[0],*draws[0]},m,shader) : nonPose(MemberView{*states[0],*draws[0]},m,shader,opt,true,(chain&~kKeySafe)==0);
      if(P.on){uint64_t t=P.clock();P.ticks[1]+=t-t0;P.calls[1]++;t0=t;}
      if(w!=MB_OK) return w;
    }
    if(!count) {   // first member: record it; its heavy checks run when a second member arrives
      states[0]=&st;draws[0]=&d;memos[0]=memo;count=1;vertices=d.v.count;withNormal=shader.vs.usesNormal;firstDeferred=true;chainDirty=0;
      return MB_OK;
    }
    if(firstDeferred) {
      w=heavyChecks(0,shader);
      if(w!=MB_OK) { count=0;vertices=0;paletteSize=0;firstDeferred=false;return w; }   // as if the first append had failed: nothing grouped
      firstDeferred=false;
    }
    states[count]=&st;draws[count]=&d;memos[count]=memo;
    w=heavyChecks(count,shader);
    if(P.on){uint64_t t=P.clock();P.ticks[2]+=t-t0;P.calls[2]++;}
    if(w!=MB_OK) return w;
    ++count;vertices+=d.v.count;withNormal=shader.vs.usesNormal;chainDirty|=dirtyIn;return MB_OK;
  }
};
} } // namespace t2p::matrixbatch
