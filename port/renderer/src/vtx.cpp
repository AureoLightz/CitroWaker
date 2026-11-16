#include "vtx.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// (Claude) Console GPU-hang bisection: true = the generated vertex shaders use dp4 on a (x, y, z, 1) copy instead of dph (same values).
bool g_shaderNoDph = false;
// (Claude v100) SCREEN texgen mapping: s = sx*(x+w) + ox*w, t = sy*(y+w) + oy*w. Default = the whole texture; the 3DS host sets it so the
// read lands in the used corner of a padded power-of-two view (the console samples NPOT views with a 256-texel row: scrambled).
float g_screenTexgen[4] = {0.5f, 0.5f, 0.0f, 0.0f};
// (Claude) Console GPU-hang bisection, round 5 (0 = normal): 1 no texture-coordinate outputs (their instructions are dropped), 2 the texture
// coordinates are declared before the colour (o1 = texcoord0, o2 = colour), 3 every shader gets one more output (an unused texcoord, zero),
// 4 shaders without texgens get a texcoord0 output (zero): three outputs on the draws that work today.
int g_shaderOutTest = 0;
namespace t2p {

// =====================================================================================================================
// Reference evaluator (GX semantics, float32)
// =====================================================================================================================
namespace {

inline float rnd(float x) { return std::floor(x + 0.5f); }
inline float mx(float a, float b) { return a > b ? a : b; }
inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Computes one half (rgb or alpha) of channel 0 as the integer byte values (stored in float).
void chanPart(const VtxChan& ch, const VtxUniforms& u, const VtxInput& in, const float e[3], const float n[3], bool alpha,
              float out[3]) {
  const int lanes = alpha ? 1 : 3, base = alpha ? 3 : 0;
  float mat[3], lacc[3];
  for (int c = 0; c < lanes; c++) {
    mat[c] = rnd(ch.matSrc ? in.clr0[base + c] : u.matColor[base + c]);
    lacc[c] = ch.enable ? rnd(ch.ambSrc ? in.clr0[base + c] : u.ambColor[base + c]) : 255.0f;
  }
  if (ch.enable) {
    for (int i = 0; i < 8; i++) {
      if (!(ch.lightMask & (1 << i))) continue;
      const VtxLight& L = u.light[i];
      float ldir[3] = {L.pos[0] - e[0], L.pos[1] - e[1], L.pos[2] - e[2]};
      float attn = 1.0f;
      if (ch.attn == 1) {  // SPOT
        float dist2 = dot3(ldir, ldir), dist = std::sqrt(dist2);
        for (float& v : ldir) v /= dist;
        attn = mx(0.0f, dot3(ldir, L.dir));
        attn = mx(0.0f, L.cosatt[0] + L.cosatt[1] * attn + L.cosatt[2] * attn * attn) /
               (L.distatt[0] + L.distatt[1] * dist + L.distatt[2] * dist2);
      } else if (ch.attn == 0) {  // SPEC
        float len = std::sqrt(dot3(ldir, ldir));
        for (float& v : ldir) v /= len;
        attn = dot3(n, ldir) >= 0.0f ? mx(0.0f, dot3(n, L.dir)) : 0.0f;
        float dk[3] = {L.distatt[0], L.distatt[1], L.distatt[2]};
        if (ch.diffuse != 0) {  // Dolphin normalises the distance polynomial when a diffuse function is also set
          float l = std::sqrt(dot3(dk, dk));
          for (float& v : dk) v /= l;
        }
        attn = mx(0.0f, L.cosatt[0] + L.cosatt[1] * attn + L.cosatt[2] * attn * attn) / (dk[0] + dk[1] * attn + dk[2] * attn * attn);
      } else {  // NONE / DIR
        float len = std::sqrt(dot3(ldir, ldir));
        for (float& v : ldir) v /= len;
      }
      float s = attn;
      if (ch.diffuse == 1) s = attn * dot3(ldir, n);
      else if (ch.diffuse == 2) s = attn * mx(0.0f, dot3(ldir, n));
      for (int c = 0; c < lanes; c++) lacc[c] += rnd(s * L.color[base + c]);
    }
  }
  for (int c = 0; c < lanes; c++) {
    float l = std::min(255.0f, mx(0.0f, lacc[c]));
    out[c] = std::floor(mat[c] * (l + (l >= 128.0f ? 1.0f : 0.0f)) / 256.0f);
  }
}

}  // namespace

VtxOutput evalVertex(const VtxSetup& s, const VtxUniforms& u, const VtxInput& in) {
  VtxOutput o;
  int mi = std::max(0, std::min(in.mtxIdx, 9));
  float e[3], n[3];
  for (int r = 0; r < 3; r++) e[r] = u.pos[mi][r][0] * in.pos[0] + u.pos[mi][r][1] * in.pos[1] + u.pos[mi][r][2] * in.pos[2] + u.pos[mi][r][3];
  for (int r = 0; r < 3; r++) n[r] = u.nrm[mi][r][0] * in.nrm[0] + u.nrm[mi][r][1] * in.nrm[1] + u.nrm[mi][r][2] * in.nrm[2];
  float nl = std::sqrt(dot3(n, n));
  for (float& v : n) v /= nl;
  for (int r = 0; r < 4; r++) o.clip[r] = u.proj[r][0] * e[0] + u.proj[r][1] * e[1] + u.proj[r][2] * e[2] + u.proj[r][3];

  float rgb[3], a[3];
  chanPart(s.color0, u, in, e, n, false, rgb);
  chanPart(s.alpha0, u, in, e, n, true, a);
  o.color[0] = rgb[0] / 255.0f; o.color[1] = rgb[1] / 255.0f; o.color[2] = rgb[2] / 255.0f; o.color[3] = a[0] / 255.0f;

  for (size_t i = 0; i < s.texgen.size() && i < 3; i++) {
    const VtxTexGen& g = s.texgen[i];
    float* t = o.tc[i];
    auto finish = [&]() {
      if (s.texScaleMask & (1u << i)) { t[0] *= u.texScale[i][0]; t[1] *= u.texScale[i][1]; }
      if (s.flipT) t[1] = t[2] - t[1];
    };
    if (g.type == 10) {
      if (g.src == 19) { t[0] = o.color[0]; t[1] = o.color[1]; t[2] = 1.0f; finish(); }
      continue;
    }
    if (g.type == 20) {   // SCREEN: the pixel's own position in the target, as (s, t, q) with s / q, t / q in 0..1 (no texScale, no flip)
      t[0] = g_screenTexgen[0] * (o.clip[0] + o.clip[3]) + g_screenTexgen[2] * o.clip[3];
      t[1] = g_screenTexgen[1] * (o.clip[1] + o.clip[3]) + g_screenTexgen[3] * o.clip[3]; t[2] = o.clip[3];
      continue;
    }
    if (g.type > 1) continue;
    float vec[4] = {0, 0, 0, 1};
    if (g.src == 0) { vec[0] = in.pos[0]; vec[1] = in.pos[1]; vec[2] = in.pos[2]; }
    else if (g.src == 1) { vec[0] = in.nrm[0]; vec[1] = in.nrm[1]; vec[2] = in.nrm[2]; }
    else if (g.src >= 4 && g.src <= 11) { vec[0] = in.tex[g.src - 4][0]; vec[1] = in.tex[g.src - 4][1]; vec[2] = 1.0f; }
    else continue;
    static const float ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    const float(*m)[4] = g.mtx == 60 ? ident : u.texMtx[std::max(0, std::min((g.mtx - 30) / 3, 9))];
    for (int r = 0; r < 2; r++) t[r] = m[r][0] * vec[0] + m[r][1] * vec[1] + m[r][2] * vec[2] + m[r][3] * vec[3];
    t[2] = g.type == 0 ? m[2][0] * vec[0] + m[2][1] * vec[1] + m[2][2] * vec[2] + m[2][3] * vec[3] : 1.0f;
    finish();
  }
  return o;
}

// =====================================================================================================================
// Packing
// =====================================================================================================================
namespace {
PVec pv(float a, float b, float c, float d) { PVec p; p.v[0] = a; p.v[1] = b; p.v[2] = c; p.v[3] = d; return p; }
}

UniformMap packUniforms(const VtxSetup& S, const VtxShader& sh, const VtxUniforms& U) {
  UniformMap m;
  packUniformsInto(m, S, sh, U);
  return m;
}

void packUniformsInto(UniformMap& m, const VtxSetup& S, const VtxShader& sh, const VtxUniforms& U) {
  const int N = S.numPosMtx;
  std::vector<PVec>& proj = m["projMtx"];
  proj.resize(4);
  for (int r = 0; r < 4; r++) proj[r] = pv(U.proj[r][0], U.proj[r][1], U.proj[r][2], U.proj[r][3]);
  std::vector<PVec>& pm = m["posMtx"];
  pm.clear();
  for (int k = 0; k < N; k++)
    for (int r = 0; r < 3; r++) pm.push_back(pv(U.pos[k][r][0], U.pos[k][r][1], U.pos[k][r][2], U.pos[k][r][3]));
  if (sh.usesNormal) {
    std::vector<PVec>& nm = m["nrmMtx"];
    nm.clear();
    for (int k = 0; k < N; k++)
      for (int r = 0; r < 3; r++) nm.push_back(pv(U.nrm[k][r][0], U.nrm[k][r][1], U.nrm[k][r][2], U.nrm[k][r][3]));
  } else {
    m.erase("nrmMtx");
  }
  m["matColor"].assign(1, pv(U.matColor[0], U.matColor[1], U.matColor[2], U.matColor[3]));
  m["ambColor"].assign(1, pv(U.ambColor[0], U.ambColor[1], U.ambColor[2], U.ambColor[3]));
  std::vector<PVec>& lv = m["light"];
  lv.assign(size_t(5 * std::max(sh.numLights, 0)), PVec{});
  for (int i = 0; i < 8; i++) {
    int sl = sh.lightSlot[i];
    if (sl < 0) continue;
    const VtxLight& L = U.light[i];
    lv[5 * sl + 0] = pv(L.color[0], L.color[1], L.color[2], L.color[3]);
    lv[5 * sl + 1] = pv(L.pos[0], L.pos[1], L.pos[2], 0);
    lv[5 * sl + 2] = pv(L.dir[0], L.dir[1], L.dir[2], 0);
    lv[5 * sl + 3] = pv(L.cosatt[0], L.cosatt[1], L.cosatt[2], 0);
    lv[5 * sl + 4] = pv(L.distatt[0], L.distatt[1], L.distatt[2], 0);
  }
  std::vector<PVec>& tv = m["texMtx"];
  tv.assign(size_t(3 * std::max(sh.numTexMtx, 0)), PVec{});
  for (size_t i = 0; i < S.texgen.size() && i < 3; i++) {
    int sl = sh.texMtxSlot[i];
    if (sl < 0) continue;
    int k = (S.texgen[i].mtx - 30) / 3;
    for (int r = 0; r < 3; r++) tv[3 * sl + r] = pv(U.texMtx[k][r][0], U.texMtx[k][r][1], U.texMtx[k][r][2], U.texMtx[k][r][3]);
  }
  std::vector<PVec>& ts = m["texScale"];
  ts.assign(size_t(std::max(sh.numTexScale, 0)), PVec{});
  for (size_t i = 0; i < S.texgen.size() && i < 3; i++)
    if (sh.texScaleSlot[i] >= 0) ts[size_t(sh.texScaleSlot[i])] = pv(U.texScale[i][0], U.texScale[i][1], 0, 0);
}

int packedUniformSlot(const std::string& name) {
  static const char* names[]={"projMtx","posMtx","nrmMtx","matColor","ambColor","light","texMtx","texScale"};
  for(int i=0;i<8;i++) if(name==names[i]) return i;
  return -1;
}

template<class Uniforms>
static void packUniformsPackedImpl(PackedUniforms& out,const VtxSetup& S,const VtxShader& sh,const Uniforms& U,const float proj[4][4],const float scale[3][2]) {
  // Copy the same float bits directly into persistent, fixed-capacity rows.
  // Projection and texture scale are supplied separately to avoid copying all
  // matrices/lights merely to override those two fields for this draw.
  auto& a=out.arrays;
  a[0].count=4; std::memcpy(a[0].rows,proj,4*sizeof(PVec));
  a[1].count=3*S.numPosMtx; std::memcpy(a[1].rows,U.pos,size_t(a[1].count)*sizeof(PVec));
  a[2].count=sh.usesNormal?3*S.numPosMtx:0;
  if(a[2].count)std::memcpy(a[2].rows,U.nrm,size_t(a[2].count)*sizeof(PVec));
  a[3].count=a[4].count=1;
  std::memcpy(a[3].rows,U.matColor,sizeof(PVec)); std::memcpy(a[4].rows,U.ambColor,sizeof(PVec));
  a[5].count=5*std::max(sh.numLights,0);
  for(int i=0;i<a[5].count;i++)a[5].rows[i]=PVec{};
  for(int i=0;i<8;i++) {
    int sl=sh.lightSlot[i];if(sl<0)continue;
    const auto& l=U.light[i];auto* dst=a[5].rows+5*sl;
    dst[0]=pv(l.color[0],l.color[1],l.color[2],l.color[3]);
    dst[1]=pv(l.pos[0],l.pos[1],l.pos[2],0);
    dst[2]=pv(l.dir[0],l.dir[1],l.dir[2],0);
    dst[3]=pv(l.cosatt[0],l.cosatt[1],l.cosatt[2],0);
    dst[4]=pv(l.distatt[0],l.distatt[1],l.distatt[2],0);
  }
  a[6].count=3*std::max(sh.numTexMtx,0);
  for(int i=0;i<a[6].count;i++)a[6].rows[i]=PVec{};
  for(size_t i=0;i<S.texgen.size()&&i<3;i++) {
    int sl=sh.texMtxSlot[i];if(sl<0)continue;
    int k=(S.texgen[i].mtx-30)/3;
    std::memcpy(a[6].rows+3*sl,U.texMtx[k],3*sizeof(PVec));
  }
  a[7].count=std::max(sh.numTexScale,0);
  for(int i=0;i<a[7].count;i++)a[7].rows[i]=PVec{};
  for(size_t i=0;i<S.texgen.size()&&i<3;i++)if(sh.texScaleSlot[i]>=0)
    a[7].rows[sh.texScaleSlot[i]]=pv(scale[i][0],scale[i][1],0,0);
}
void packUniformsPacked(PackedUniforms& out,const VtxSetup& S,const VtxShader& sh,const VtxUniforms& U,const float proj[4][4],const float scale[3][2]) {packUniformsPackedImpl(out,S,sh,U,proj,scale);}
void packUniformsPacked(PackedUniforms& out,const VtxSetup& S,const VtxShader& sh,const VtxUniformView& U,const float proj[4][4],const float scale[3][2]) {packUniformsPackedImpl(out,S,sh,U,proj,scale);}


void packInputs(const VtxSetup& S, const VtxInput& I, PVec in[16]) {
  if (S.inPos >= 0) in[S.inPos] = pv(I.pos[0], I.pos[1], I.pos[2], 1);
  if (S.inNrm >= 0) in[S.inNrm] = pv(I.nrm[0], I.nrm[1], I.nrm[2], 1);
  if (S.inClr0 >= 0) in[S.inClr0] = pv(I.clr0[0], I.clr0[1], I.clr0[2], I.clr0[3]);
  for (int t = 0; t < 8; t++) if (S.inTex[t] >= 0) in[S.inTex[t]] = pv(I.tex[t][0], I.tex[t][1], 0, 1);
  if (S.inMtxIdx >= 0) in[S.inMtxIdx] = pv(float(3 * I.mtxIdx), 0, 0, 1);
}

// =====================================================================================================================
// Shader generator
// =====================================================================================================================
namespace {

struct Asm {
  std::string text;
  int count = 0;
  void raw(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    text += buf;
    text += '\n';
  }
  void i(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_shaderOutTest == 1 && std::strstr(buf, "outtc")) return;
    text += "\t";
    text += buf;
    text += '\n';
    count++;
  }
};

std::string u(int n) { return std::to_string(n); }

struct Gen {
  const VtxSetup& S;
  VtxShader R;
  Asm A;
  Gen(const VtxSetup& s) : S(s) {}

  void note(const std::string& m, bool approx) {
    R.notes.push_back(m);
    if (approx) R.exact = false;
  }

  std::string matSrcReg() const { return "matColor"; }

  // Emits the light accumulation for one light into lacc lanes (r5), scalars in r4/r7, lane = "xyz" or "w".
  void light(const VtxChan& ch, int slot, const char* lane) {
    const std::string Lcol = "light[" + u(5 * slot) + "]", Lpos = "light[" + u(5 * slot + 1) + "]",
                      Ldir = "light[" + u(5 * slot + 2) + "]", Lcos = "light[" + u(5 * slot + 3) + "]",
                      Ldst = "light[" + u(5 * slot + 4) + "]";
    A.i("add r3.xyz, %s, -r1", Lpos.c_str());   // ldir = lightPos - eyePos
    A.i("dp3 r4.x, r3, r3");                     // dist^2
    A.i("rsq r4.y, r4.x");                       // 1 / dist
    A.i("mul r3.xyz, r3, r4.yyyy");              // normalised ldir
    if (ch.attn == 1) {                          // SPOT
      A.i("mul r4.z, r4.x, r4.y");               // dist
      A.i("dp3 r4.w, %s, r3", Ldir.c_str());
      A.i("max r4.w, kZero, r4.w");
      A.i("mul r7.x, %s.zzzz, r4.wwww", Lcos.c_str());
      A.i("add r7.x, %s.yyyy, r7.xxxx", Lcos.c_str());
      A.i("mul r7.x, r7.xxxx, r4.wwww");
      A.i("add r7.x, %s.xxxx, r7.xxxx", Lcos.c_str());
      A.i("max r7.x, kZero, r7.xxxx");           // max(0, cosAtt)
      A.i("mul r7.y, %s.yyyy, r4.zzzz", Ldst.c_str());
      A.i("mul r7.z, %s.zzzz, r4.xxxx", Ldst.c_str());
      A.i("add r7.y, r7.yyyy, r7.zzzz");
      A.i("add r7.y, %s.xxxx, r7.yyyy", Ldst.c_str());
      A.i("rcp r7.y, r7.y");
      A.i("mul r7.x, r7.x, r7.y");               // attn
    } else if (ch.attn == 0) {                   // SPEC
      A.i("dp3 r4.w, r2, r3");                   // n . ldir
      A.i("sge r4.z, r4.w, kZero");
      A.i("dp3 r4.w, %s, r2", Ldir.c_str());
      A.i("max r4.w, kZero, r4.w");
      A.i("mul r4.w, r4.w, r4.z");               // attn = (n.ldir >= 0) ? max(0, n.dir) : 0
      A.i("mul r7.x, %s.zzzz, r4.wwww", Lcos.c_str());
      A.i("add r7.x, %s.yyyy, r7.xxxx", Lcos.c_str());
      A.i("mul r7.x, r7.xxxx, r4.wwww");
      A.i("add r7.x, %s.xxxx, r7.xxxx", Lcos.c_str());
      A.i("max r7.x, kZero, r7.xxxx");
      A.i("mul r7.y, %s.zzzz, r4.wwww", Ldst.c_str());
      A.i("add r7.y, %s.yyyy, r7.yyyy", Ldst.c_str());
      A.i("mul r7.y, r7.yyyy, r4.wwww");
      A.i("add r7.y, %s.xxxx, r7.yyyy", Ldst.c_str());
      A.i("rcp r7.y, r7.y");
      A.i("mul r7.x, r7.x, r7.y");
    } else {                                      // NONE / DIR
      A.i("mov r7.x, kOne");
    }
    if (ch.diffuse == 1 || ch.diffuse == 2) {
      A.i("dp3 r7.z, r3, r2");
      if (ch.diffuse == 2) A.i("max r7.z, kZero, r7.zzzz");
      A.i("mul r7.x, r7.x, r7.z");
    }
    A.i("mul r8.%s, %s, r7.xxxx", lane, Lcol.c_str());
    A.i("add r8.%s, kHalf, r8", lane);
    A.i("flr r8.%s, r8", lane);
    A.i("add r5.%s, r5, r8", lane);
  }

  // One half of the channel. Result lands in r9.<lane> as k/255.
  void part(const VtxChan& ch, const char* lane, bool alpha) {
    (void)alpha;
    if (!ch.enable) {
      A.i("mul r9.%s, kInv255, r6", lane);
      return;
    }
    A.i("mov r5.%s, %s", lane, ch.ambSrc ? "inClr0" : "ambColor");
    for (int i = 0; i < 8; i++)
      if (ch.lightMask & (1 << i)) light(ch, R.lightSlot[i], lane);
    A.i("max r5.%s, kZero, r5", lane);
    A.i("min r5.%s, k255, r5", lane);
    A.i("sge r7.%s, r5, k128", lane);
    A.i("add r7.%s, r5, r7", lane);
    A.i("mul r7.%s, r6, r7", lane);
    A.i("mul r7.%s, kInv256, r7", lane);
    A.i("flr r7.%s, r7", lane);
    A.i("mul r9.%s, kInv255, r7", lane);
  }

  void texgen(int idx, const VtxTexGen& g) {
    const std::string dest = "outtc" + u(idx);
    if (g.type == 20) {   // SCREEN (unit 0 only, checked in run()): the clip position sits in r14
      A.i("add r10.xy, r14, r14.wwww");
      A.i("mul r10.xy, kScr, r10");
      if (g_screenTexgen[2] != 0.0f || g_screenTexgen[3] != 0.0f) {
        A.i("mul r10.zw, kScr, r14.wwww");
        A.i("add r10.xy, r10, r10.zwzw");
      }
      A.i("mov outtc0.xy, r10");
      A.i("mov outtc0.z, r14.wwww");
      R.tc0Projective = true;
      return;
    }
    if (g.type == 10) {
      if (g.src == 19) {
        const int ss = R.texScaleSlot[idx];
        if (ss >= 0) {
          A.i("mul r10.xy, texScale[%d], r9", ss);
          if (S.flipT) A.i("add r10.y, kOne, -r10.yyyy");
          A.i("mov %s.xy, r10", dest.c_str());
        } else {
          // PICA outputs are write-only and each lane must be written once.
          A.i("mov r10.xy, r9");
          if (S.flipT) A.i("add r10.y, kOne, -r9.yyyy");
          A.i("mov %s.xy, r10", dest.c_str());
        }
      }
      else { A.i("mov %s.xy, kZero", dest.c_str()); note("texgen " + u(idx) + ": SRTG from COLOR1 is not supported (channel 1 is not generated)", true); }
      return;
    }
    if (g.type > 1) {
      A.i("mov %s.xy, kZero", dest.c_str());
      note("texgen " + u(idx) + ": bump/emboss texgen type " + u(g.type) + " is not supported", true);
      return;
    }
    if (g.normalize || g.postMtx != 125) note("texgen " + u(idx) + ": dual-transform (normalize / post matrix) is ignored", true);
    std::string src;
    bool texSrc = false;
    if (g.src == 0) src = "inPos";
    else if (g.src == 1) src = "inNrm";
    else if (g.src >= 4 && g.src <= 11) { src = "inTex" + u(g.src - 4); texSrc = true; }
    else {
      A.i("mov %s.xy, kZero", dest.c_str());
      note("texgen " + u(idx) + ": source " + u(g.src) + " is not supported", true);
      return;
    }
    const bool ident = g.mtx == 60;
    const int slot = R.texMtxSlot[idx];
    // r10.xyz = (s, t, q)
    if (ident) {
      A.i("mov r10.xy, %s", src.c_str());
      if (g.type == 0 && !texSrc) A.i("mov r10.z, %s.zzzz", src.c_str());
      else A.i("mov r10.z, kOne");
    } else if (!texSrc) {
      if (g_shaderNoDph) {
        A.i("mov r11.xyz, %s", src.c_str());
        A.i("mov r11.w, kOne");
        A.i("dp4 r10.x, texMtx[%d], r11", 3 * slot);
        A.i("dp4 r10.y, texMtx[%d], r11", 3 * slot + 1);
        if (g.type == 0) A.i("dp4 r10.z, texMtx[%d], r11", 3 * slot + 2);
        else A.i("mov r10.z, kOne");
      } else {
      A.i("dph r10.x, %s, texMtx[%d]", src.c_str(), 3 * slot);
      A.i("dph r10.y, %s, texMtx[%d]", src.c_str(), 3 * slot + 1);
      if (g.type == 0) A.i("dph r10.z, %s, texMtx[%d]", src.c_str(), 3 * slot + 2);
      else A.i("mov r10.z, kOne");
      }
    } else {
      A.i("mov r11.xy, %s", src.c_str());
      A.i("mov r11.zw, kOne");
      A.i("dp4 r10.x, texMtx[%d], r11", 3 * slot);
      A.i("dp4 r10.y, texMtx[%d], r11", 3 * slot + 1);
      if (g.type == 0) A.i("dp4 r10.z, texMtx[%d], r11", 3 * slot + 2);
      else A.i("mov r10.z, kOne");
    }
    const bool qIsOne = g.type == 1 || (texSrc && ident);
    if (S.dbgTc == idx) A.i("mov r13.xyz, r10");
    if (R.texScaleSlot[idx] >= 0) A.i("mul r10.xy, texScale[%d], r10", R.texScaleSlot[idx]);   // the image fills only part of the PICA texture
    if (S.flipT) A.i("add r10.y, r10.zzzz, -r10.yyyy");   // t' = q - t: PICA textures are upside down relative to GX
    if (idx == 0) {
      A.i("mov outtc0.xy, r10");
      if (!qIsOne) { A.i("mov outtc0.z, r10.zzzz"); R.tc0Projective = true; }
    } else if (qIsOne) {
      A.i("mov %s.xy, r10", dest.c_str());
    } else {
      A.i("rcp r12.x, r10.z");
      A.i("mul %s.xy, r10, r12.xxxx", dest.c_str());
      note("texgen " + u(idx) + ": projective (q != 1) on texture unit " + u(idx) + ": PICA only projects unit 0, so s/q and t/q are taken per vertex (perspective-incorrect)", true);
    }
  }

  VtxShader run() {
    if (S.texgen.size() > 3) { R.error = "more than 3 texgens: PICA has 3 texcoord outputs"; return R; }
    if (S.numPosMtx < 1 || S.numPosMtx > 10) { R.error = "numPosMtx must be 1..10"; return R; }
    if (S.numPosMtx > 1 && S.inMtxIdx < 0) { R.error = "numPosMtx > 1 needs a matrix-index attribute"; return R; }
    if (S.inPos < 0 && S.dbgClipReg < 0) { R.error = "no position attribute"; return R; }

    // ---- analysis
    bool srtgColor = false;
    for (const VtxTexGen& g : S.texgen) if (g.type == 10 && g.src == 19) srtgColor = true;
    const bool computeColor = (S.needColor || srtgColor) && S.dbgTc < 0;   // no lighting work when nothing reads the color
    unsigned lightUnion = 0;
    bool litNormal = false;
    if (computeColor)
      for (const VtxChan* c : {&S.color0, &S.alpha0})
        if (c->enable && c->lightMask) { lightUnion |= c->lightMask; litNormal = true; }
    R.usesNormal = litNormal;
    if (litNormal && S.inNrm < 0) { R.error = "lighting needs a normal attribute"; return R; }
    bool needMat = false, needAmb = false, needClrAttr = false;
    for (const VtxChan* c : {&S.color0, &S.alpha0}) {
      if (c->matSrc) needClrAttr = true; else needMat = true;
      if (c->enable) { if (c->ambSrc) needClrAttr = true; else needAmb = true; }
    }
    if (computeColor && needClrAttr && S.inClr0 < 0) { R.error = "vertex color is a material/ambient source but there is no color attribute"; return R; }
    bool screen = false;
    for (size_t i = 0; i < S.texgen.size(); i++) {
      const VtxTexGen& g = S.texgen[i];
      if (g.type == 20) {
        if (i != 0) { R.error = "the SCREEN texgen reads the clip position as a projective coordinate: only texture unit 0 projects"; return R; }
        if (S.dbgClipReg >= 0) { R.error = "the SCREEN texgen cannot be combined with a debug clip register"; return R; }
        screen = true;
      }
      if (g.type <= 1 && g.src >= 4 && g.src <= 11 && S.inTex[g.src - 4] < 0) { R.error = "texgen " + u(int(i)) + " reads a texcoord attribute that is not present"; return R; }
      if (g.type <= 1 && g.src == 1 && S.inNrm < 0) { R.error = "texgen from NRM needs a normal attribute"; return R; }
      if (g.type <= 1 && g.mtx != 60 && (g.mtx < 30 || g.mtx > 57 || (g.mtx - 30) % 3)) { R.error = "texgen " + u(int(i)) + ": bad matrix id " + u(g.mtx); return R; }
    }
    for (int i = 0; i < 8; i++) if (lightUnion & (1u << i)) R.lightSlot[i] = R.numLights++;
    for (size_t i = 0; i < S.texgen.size(); i++) {
      const VtxTexGen& g = S.texgen[i];
      if (g.type <= 1 && g.mtx != 60) R.texMtxSlot[i] = R.numTexMtx++;
    }
    for (size_t i = 0; i < S.texgen.size() && i < 3; i++)
      if (S.texScaleMask & (1u << i)) R.texScaleSlot[i] = R.numTexScale++;
    for (const VtxChan* c : {&S.color0, &S.alpha0}) {
      if (computeColor && c->enable && c->attn == 0 && c->diffuse != 0 && c->lightMask)
        note("SPEC attenuation with a diffuse function: GX normalises the distance polynomial; the generator does not (the SDK never combines them)", true);
    }
    if (S.color0.enable != S.alpha0.enable) {}  // handled per half

    // ---- header
    int N = S.numPosMtx;
    A.raw("; generated by tev2pica vtx.cpp -- GX transform unit -> PICA200 vertex shader");
    A.raw(".fvec projMtx[4]");
    A.raw(".fvec posMtx[%d]", 3 * N);
    if (litNormal) A.raw(".fvec nrmMtx[%d]", 3 * N);
    if (computeColor && needMat) A.raw(".fvec matColor");
    if (computeColor && needAmb && (S.color0.enable || S.alpha0.enable)) A.raw(".fvec ambColor");
    if (R.numLights && computeColor) A.raw(".fvec light[%d]", 5 * R.numLights);
    if (R.numTexMtx) A.raw(".fvec texMtx[%d]", 3 * R.numTexMtx);
    if (R.numTexScale) A.raw(".fvec texScale[%d]", R.numTexScale);
    if (S.dbgTc >= 0) A.raw(".fvec dbgScale");
    A.raw(".constf kC0(0.0, 1.0, 0.5, 255.0)");
    A.raw(".constf kC1(128.0, 0.00390625, 0.00392156862745098, 0.0)");
    if (screen) A.raw(".constf kScr(%.9g, %.9g, %.9g, %.9g)", g_screenTexgen[0], g_screenTexgen[1], g_screenTexgen[2], g_screenTexgen[3]);
    A.raw(".alias kZero kC0.xxxx");
    A.raw(".alias kOne kC0.yyyy");
    A.raw(".alias kHalf kC0.zzzz");
    A.raw(".alias k255 kC0.wwww");
    A.raw(".alias k128 kC1.xxxx");
    A.raw(".alias kInv256 kC1.yyyy");
    A.raw(".alias kInv255 kC1.zzzz");
    A.raw(".in inPos v%d", S.inPos >= 0 ? S.inPos : 0);
    R.usesPos = true;
    if (S.dbgClipReg >= 0) A.raw(".in inClip v%d", S.dbgClipReg);
    if (S.inNrm >= 0 && (litNormal || std::any_of(S.texgen.begin(), S.texgen.end(), [](const VtxTexGen& g) { return g.type <= 1 && g.src == 1; })))
    { A.raw(".in inNrm v%d", S.inNrm); R.usesNrmAttr = true; }
    if (S.inClr0 >= 0 && computeColor && needClrAttr) { A.raw(".in inClr0 v%d", S.inClr0); R.usesClrAttr = true; }
    if (N > 1) { A.raw(".in inMtxIdx v%d", S.inMtxIdx); R.usesMtxIdxAttr = true; }
    {
      bool used[8] = {};
      for (const VtxTexGen& g : S.texgen) if (g.type <= 1 && g.src >= 4 && g.src <= 11) used[g.src - 4] = true;
      for (int t = 0; t < 8; t++) if (used[t]) { A.raw(".in inTex%d v%d", t, S.inTex[t]); R.usesTexAttr[t] = true; }
    }
    A.raw(".out outpos position");
    if (g_shaderOutTest != 2) A.raw(".out outclr color");
    if (g_shaderOutTest != 1) for (size_t i = 0; i < S.texgen.size(); i++) A.raw(".out outtc%d texcoord%d.xy", int(i), int(i));
    if (g_shaderOutTest == 2) A.raw(".out outclr color");
    const bool hwExtraOut = (g_shaderOutTest == 3 && S.texgen.size() < 3) || (g_shaderOutTest == 4 && S.texgen.empty());
    if (hwExtraOut) A.raw(".out outhwx texcoord%d.xy", int(S.texgen.size()));
    bool tc0w = false;
    if (!S.texgen.empty()) {
      const VtxTexGen& g0 = S.texgen[0];
      bool texSrc0 = g0.src >= 4 && g0.src <= 11;
      tc0w = (g0.type == 0 && !(texSrc0 && g0.mtx == 60)) || g0.type == 20;
      if (tc0w && g_shaderOutTest != 1) A.raw(".out - texcoord0w outtc0.z");
    }
    A.raw("");
    A.raw(".proc main");
    if (hwExtraOut) A.i("mov outhwx, kZero");

    // ---- position
    if (N > 1) A.i("mova a0.x, inMtxIdx");
    auto row = [&](const char* arr, int r) { return N > 1 ? std::string(arr) + "[a0.x" + (r ? "+" + u(r) : "") + "]" : std::string(arr) + "[" + u(r) + "]"; };
    if (g_shaderNoDph) {
      A.i("mov r11.xyz, inPos");
      A.i("mov r11.w, kOne");
      A.i("dp4 r1.x, %s, r11", row("posMtx", 0).c_str());
      A.i("dp4 r1.y, %s, r11", row("posMtx", 1).c_str());
      A.i("dp4 r1.z, %s, r11", row("posMtx", 2).c_str());
      A.i("mov r1.w, kOne");
    } else {
    A.i("dph r1.x, inPos, %s", row("posMtx", 0).c_str());
    A.i("dph r1.y, inPos, %s", row("posMtx", 1).c_str());
    A.i("dph r1.z, inPos, %s", row("posMtx", 2).c_str());
    }
    if (S.dbgClipReg >= 0) {
      A.i("mov outpos, inClip");
    } else if (screen) {   // output registers cannot be read back: keep a copy for the SCREEN texgen
      for (int r = 0; r < 4; r++) A.i(g_shaderNoDph ? "dp4 r14.%c, projMtx[%d], r1" : "dph r14.%c, r1, projMtx[%d]", "xyzw"[r], r);
      A.i("mov outpos, r14");
    } else {
      for (int r = 0; r < 4; r++) A.i(g_shaderNoDph ? "dp4 outpos.%c, projMtx[%d], r1" : "dph outpos.%c, r1, projMtx[%d]", "xyzw"[r], r);
    }

    // ---- normal
    if (litNormal && computeColor) {
      A.i("dp3 r2.x, %s, inNrm", row("nrmMtx", 0).c_str());
      A.i("dp3 r2.y, %s, inNrm", row("nrmMtx", 1).c_str());
      A.i("dp3 r2.z, %s, inNrm", row("nrmMtx", 2).c_str());
      A.i("dp3 r4.x, r2, r2");
      A.i("rsq r4.x, r4.x");
      A.i("mul r2.xyz, r2, r4.xxxx");
    }

    // ---- color
    if (computeColor) {
      if (S.color0.matSrc == S.alpha0.matSrc) A.i("mov r6, %s", S.color0.matSrc ? "inClr0" : "matColor");
      else {
        A.i("mov r6.xyz, %s", S.color0.matSrc ? "inClr0" : "matColor");
        A.i("mov r6.w, %s", S.alpha0.matSrc ? "inClr0" : "matColor");
      }
      part(S.color0, "xyz", false);
      part(S.alpha0, "w", true);
    }
    if (S.dbgTc < 0) {
      if (computeColor) A.i("mov outclr, r9");
      else A.i("mov outclr, kOne");
    }

    // ---- texgens
    for (size_t i = 0; i < S.texgen.size(); i++) texgen(int(i), S.texgen[i]);
    // Fully retire each enabled physical output vector on real PICA hardware.
    // These lanes have no mapped semantics; defined position/colour/UV are unchanged.
    for (size_t i = 0; i < S.texgen.size(); i++) {
      if (i == 0 && R.tc0Projective) A.i("mov outtc0.w, kZero");
      else A.i("mov outtc%d.zw, kZero", int(i));
    }
    if (S.dbgTc >= 0) {
      A.i("mul r12.xyz, dbgScale.xxxx, r13");
      A.i("add r12.xyz, dbgScale.yyyy, r12");
      A.i("mov r12.w, kOne");
      A.i("mov outclr, r12");
    }
    A.i("end");
    A.raw(".end");

    R.tc0Projective = R.tc0Projective && tc0w;
    R.uniformRegs = 4 + 3 * N + (litNormal ? 3 * N : 0) + (computeColor && needMat ? 1 : 0) +
                    (computeColor && needAmb ? 1 : 0) + (computeColor ? 5 * R.numLights : 0) + 3 * R.numTexMtx + R.numTexScale + (S.dbgTc >= 0 ? 1 : 0);
    R.source = A.text;
    R.ok = true;
    return R;
  }
};

}  // namespace

VtxShader genVertexShader(const VtxSetup& s) {
  Gen g(s);
  return g.run();
}

}  // namespace t2p
