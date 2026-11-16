#include "pipeline.h"
#include <cstring>
#include <cfenv>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace t2p {

// ------------------------------------------------------------------ tests / alpha
uint8_t picaFuncFromGx(int c) {
  static const uint8_t m[8] = {PF_NEVER, PF_LESS, PF_EQUAL, PF_LEQUAL, PF_GREATER, PF_NOTEQUAL, PF_GEQUAL, PF_ALWAYS};
  return m[c & 7];
}

static bool gxCmp(int c, int a, int ref) {
  switch (c & 7) {
    case 0: return false;
    case 1: return a < ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a > ref;
    case 5: return a != ref;
    case 6: return a >= ref;
    default: return true;
  }
}

static bool picaPass(int func, int a, int ref) {
  switch (func) {
    case PF_NEVER: return false;
    case PF_ALWAYS: return true;
    case PF_EQUAL: return a == ref;
    case PF_NOTEQUAL: return a != ref;
    case PF_LESS: return a < ref;
    case PF_LEQUAL: return a <= ref;
    case PF_GREATER: return a > ref;
    default: return a >= ref;
  }
}

bool gxAlphaPasses(const GxAlphaCompare& g, int a) {
  bool x = gxCmp(g.comp0, a, g.ref0), y = gxCmp(g.comp1, a, g.ref1);
  switch (g.op & 3) {
    case 0: return x && y;
    case 1: return x || y;
    case 2: return x != y;
    default: return x == y;
  }
}

static PicaAlphaTest alphaFromGxSearch(const GxAlphaCompare& g);
// (Claude v105) memo: the search below is 6 x 256 x 256 compares (~1 ms on the ARM11, half of every draw compile); a game uses a handful of alpha tests
PicaAlphaTest alphaFromGx(const GxAlphaCompare& g) {
  struct Memo { bool valid = false; uint8_t k[5]; PicaAlphaTest r; };
  static thread_local Memo memo[16];
  static thread_local unsigned next = 0;
  const uint8_t k[5] = {uint8_t(g.comp0), uint8_t(g.ref0), uint8_t(g.op), uint8_t(g.comp1), uint8_t(g.ref1)};
  for (Memo& m : memo) if (m.valid && std::memcmp(m.k, k, 5) == 0) return m.r;
  Memo& m = memo[next++ & 15];
  m.r = alphaFromGxSearch(g); std::memcpy(m.k, k, 5); m.valid = true;
  return m.r;
}
static PicaAlphaTest alphaFromGxSearch(const GxAlphaCompare& g) {
  PicaAlphaTest r;
  bool T[256];
  int pass = 0;
  for (int a = 0; a < 256; a++) pass += (T[a] = gxAlphaPasses(g, a));
  if (pass == 256) return r;  // always passes: test off
  r.enable = true;
  if (pass == 0) { r.func = PF_NEVER; r.note = "alpha test: nothing passes"; return r; }
  static const uint8_t order[6] = {PF_GEQUAL, PF_LEQUAL, PF_GREATER, PF_LESS, PF_EQUAL, PF_NOTEQUAL};
  int best = 257;
  // (Claude v105) same pick as the full search: a candidate counts only when strictly better, so a partial count at best or above is dropped,
  // and the first exact candidate is final
  for (uint8_t f : order) {
    for (int ref = 0; ref < 256 && best != 0; ref++) {
      int bad = 0;
      for (int a = 0; a < 256 && bad < best; a++) bad += picaPass(f, a, ref) != T[a];
      if (bad < best) { best = bad; r.func = f; r.ref = uint8_t(ref); }
    }
    if (best == 0) break;
  }
  r.mismatches = best;
  r.exact = best == 0;
  if (!r.exact) {
    char b[160];
    std::snprintf(b, sizeof b, "alpha test: GX pass set is not a single PICA compare; closest is func %d ref %d (%d of 256 alpha values differ)",
                  r.func, r.ref, best);
    r.note = b;
  }
  return r;
}

// ------------------------------------------------------------------ blend
static uint8_t blendFactor(int gx, bool isSrc) {
  switch (gx & 7) {
    case 0: return 0;
    case 1: return 1;
    case 2: return isSrc ? 4 : 2;   // GX_BL_SRCCLR / DSTCLR share a code: the dst-color factor for the source, the src-color factor for the dest
    case 3: return isSrc ? 5 : 3;
    case 4: return 6;
    case 5: return 7;
    case 6: return 8;
    default: return 9;
  }
}

PicaBlend blendFromGx(const GxBlend& g) {
  PicaBlend r;
  switch (g.mode & 3) {
    case 0: break;
    case 1:
      r.srcRgb = r.srcAlpha = blendFactor(g.src, true);
      r.dstRgb = r.dstAlpha = blendFactor(g.dst, false);
      break;
    case 2: {
      static const uint8_t m[16] = {0, 1, 2, 3, 13, 6, 11, 9, 10, 12, 7, 14, 5, 15, 8, 4};  // GX_LO_* -> GPU_LOGICOP
      if ((g.logic & 15) != 3) { r.useLogicOp = true; r.logicOp = m[g.logic & 15]; r.blendsAnything = true; }
      break;
    }
    default:  // GX_BM_SUBTRACT: dst - src
      r.eqRgb = r.eqAlpha = 2;
      r.srcRgb = r.srcAlpha = r.dstRgb = r.dstAlpha = 1;
      break;
  }
  if (!r.useLogicOp && (r.srcRgb != 1 || r.dstRgb != 0 || r.eqRgb != 0)) r.blendsAnything = true;
  for (uint8_t f : {r.srcRgb, r.dstRgb}) r.needsDstAlpha |= (f == 8 || f == 9);
  if (r.needsDstAlpha) r.note = "blend reads destination alpha: the target must be RGBA and cleared to the alpha a GC RGB8 target would read (1.0)";
  return r;
}

PicaDepth depthFromGx(const GxZMode& z) {
  PicaDepth d;
  if (z.compare) { d.testEnable = true; d.func = picaFuncFromGx(z.func); d.write = z.update; }
  else { d.testEnable = z.update; d.func = PF_ALWAYS; d.write = z.update; }
  return d;
}

// ------------------------------------------------------------------ depth map
bool g_picaDepthFold = true;
bool g_picaDepthReverse = true;   // (Claude v107) see pipeline.h
uint8_t picaMirrorDepthFunc(uint8_t f) {
  switch (f) { case PF_LESS: return PF_GREATER; case PF_LEQUAL: return PF_GEQUAL; case PF_GREATER: return PF_LESS; case PF_GEQUAL: return PF_LEQUAL; default: return f; }
}
bool g_picaForceZDepth = false;   // (Claude v93) see pipeline.h   // (Claude v90) see pipeline.h
PicaDepthMap depthMapZ() { return PicaDepthMap{true, kPicaDepthMax, kPicaDepthMax, 0xFFFFFF}; }
PicaDepthMap depthMapW(float rangeD) { return PicaDepthMap{false, 0.0f, kPicaDepthMax / (rangeD > 0 ? rangeD : 1.0f), 0xFFFFFF}; }

float picaDepthValue(const PicaDepthMap& m, float zOverW, float wClip) {
  float d = m.zBuffer ? zOverW * m.scale + m.offset : zOverW * wClip * m.scale + wClip * m.offset;
  return d < 0 ? 0.0f : (d > 1 ? 1.0f : d);
}
uint32_t picaDepth24(const PicaDepthMap& m, float zOverW, float wClip) {
  return uint32_t(std::floor(double(picaDepthValue(m, zOverW, wClip)) * 0xFFFFFF + 0.5));
}

// ------------------------------------------------------------------ fog
float fogFactorGx(uint8_t type, float t) {
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  switch (type & 7) {
    case 4: return 1.0f - std::exp2(-8.0f * t);
    case 5: return 1.0f - std::exp2(-8.0f * t * t);
    case 6: return std::exp2(-8.0f * (1.0f - t));
    case 7: { float u = 1.0f - t; return std::exp2(-8.0f * u * u); }
    default: return t;
  }
}

static float exactFog(const GxFog& g, float d) {
  float span = g.endZ - g.startZ;
  float t = span > 0 ? (d - g.startZ) / span : (d >= g.startZ ? 1.0f : 0.0f);
  return 1.0f - fogFactorGx(g.type, t);  // PICA convention: 1 = no fog
}

float picaFogEval(const PicaFog& f, float depth, float lutScale) {
  float idx = depth * 128.0f;
  float fi = std::floor(idx);
  fi = fi < 0 ? 0 : (fi > 127 ? 127 : fi);
  float frac = idx - fi;
  uint32_t e = f.lut[int(fi)];
  float value = float((e >> 13) & 0x7FF) / lutScale;
  int32_t diff = int32_t(e & 0x1FFF);
  if (diff & 0x1000) diff -= 0x2000;
  float v = value + float(diff) / lutScale * frac;
  return v < 0 ? 0 : (v > 1 ? 1 : v);
}

float fogDepthRange(const GxFog& g,float projFar,bool shrink,float forceRange) {
  if(g.type==0 || (g.type&8))return 1.0f;
  float span = g.endZ - g.startZ;
  float D = projFar > 0 ? projFar : g.farZ;
  if (forceRange > 0) {
    D = forceRange;
  } else if (shrink) {
    // LUT cells are D/128 wide. Keep ~32 cells across the ramp (a kink inside a cell costs ~slope*cell/4), but never cut the ramp off (D >= endZ).
    float want = span > 0 ? std::max(g.endZ, 4.0f * span) : std::max(g.startZ * 1.25f, 1.0f);  // a zero-width ramp is a step: keep it inside the LUT
    D = std::min(D, want);
  }
  return D;
}

PicaFog FogCompileCache::get(const GxFog& g,float projFar,bool shrink,float forceRange) {
  uint32_t mode=0;
#if defined(__3DS__)
  asm volatile("vmrs %0, fpscr" : "=r"(mode));
  if(mode&0x9f00u)return fogFromGx(g,projFar,shrink,forceRange);
  mode&=0x07ffff00u;
#elif defined(__SSE__)
  mode=_mm_getcsr();
  if((mode&0x1f80u)!=0x1f80u)return fogFromGx(g,projFar,shrink,forceRange);
  mode&=~0x3fu;
#else
  mode=uint32_t(std::fegetround());
#endif
  uint32_t key[9]={uint32_t(g.type),uint32_t(shrink),mode};
  const float values[]={g.startZ,g.endZ,g.nearZ,g.farZ,projFar,forceRange};
  std::memcpy(key+3,values,sizeof values);
  Entry* replace=&entries_[0];
  for(auto& e:entries_) {
    if(e.valid && !std::memcmp(e.key,key,sizeof key)) {
      e.age=++age_;PicaFog result=e.value;
      result.color=uint32_t(g.r)|(uint32_t(g.g)<<8)|(uint32_t(g.b)<<16);
      return result;
    }
    if(!e.valid || (replace->valid && e.age<replace->age))replace=&e;
  }
  replace->value=fogFromGx(g,projFar,shrink,forceRange);
  std::memcpy(replace->key,key,sizeof key);replace->valid=true;replace->age=++age_;
  return replace->value;
}

PicaFog fogFromGx(const GxFog& g, float projFar, bool shrink, float forceRange) {
  PicaFog f;
  f.color = uint32_t(g.r) | (uint32_t(g.g) << 8) | (uint32_t(g.b) << 16);
  if (g.type == 0) return f;
  if (g.type & 8) {
    f.exact = false;
    f.note = "orthographic fog is not implemented (needs Z-buffer depth); fog disabled for this draw";
    return f;
  }
  f.enable = true;
  float D = fogDepthRange(g,projFar,shrink,forceRange);
  if (D < g.endZ) f.note = "fog ramp is cut off by the projection far plane";
  f.depthRange = D;
  f.depthScale = 0.0f;
  f.depthOffset = kPicaDepthMax / D;
  const float lutD = D / kPicaDepthMax;   // (Claude v84) the W map ends at kPicaDepthMax, so the 128 LUT cells span D / kPicaDepthMax
  f.depthSaturates = D < projFar;
  if (f.depthSaturates)
    f.note += (f.note.empty() ? "" : "; ") + std::string("W-buffer depth saturates beyond the fog range (everything past it is fully fogged)");

  int q[129];
  for (int i = 0; i <= 128; i++) {
    float v = exactFog(g, lutD * float(i) / 128.0f);
    q[i] = int(std::floor(v * 2048.0f + 0.5f));
    q[i] = std::max(0, std::min(2048, q[i]));
  }
  for (int i = 0; i < 128; i++) {
    uint32_t val = uint32_t(std::min(q[i], 0x7FF));
    int diff = q[i + 1] - std::min(q[i], 0x7FF);
    f.lut[i] = (uint32_t(diff) & 0x1FFF) | (val << 13);
  }
  // worst error over the covered range
  float worst = 0;
  for (int k = 0; k <= 8192; k++) {
    float depth = float(k) / 8192.0f;
    if (depth > kPicaDepthMax) break;
    worst = std::max(worst, std::fabs(picaFogEval(f, depth) - exactFog(g, depth * lutD)));
  }
  f.maxErr255 = worst * 255.0f;
  return f;
}

// ------------------------------------------------------------------ logs
std::string describe(const GxAlphaCompare& g, const PicaAlphaTest& p) {
  char b[200];
  static const char* names[8] = {"NEVER", "LESS", "EQUAL", "LEQUAL", "GREATER", "NEQUAL", "GEQUAL", "ALWAYS"};
  static const char* ops[4] = {"AND", "OR", "XOR", "XNOR"};
  static const char* pf[8] = {"NEVER", "ALWAYS", "EQUAL", "NOTEQUAL", "LESS", "LEQUAL", "GREATER", "GEQUAL"};
  if (!p.enable) std::snprintf(b, sizeof b, "(%s %d %s %s %d) -> off", names[g.comp0 & 7], g.ref0, ops[g.op & 3], names[g.comp1 & 7], g.ref1);
  else std::snprintf(b, sizeof b, "(%s %d %s %s %d) -> %s %d%s", names[g.comp0 & 7], g.ref0, ops[g.op & 3], names[g.comp1 & 7], g.ref1,
                     pf[p.func & 7], p.ref, p.exact ? "" : "  [approx]");
  return b;
}

std::string describe(const GxBlend& g, const PicaBlend& p) {
  char b[200];
  if (p.useLogicOp) std::snprintf(b, sizeof b, "logic op %d -> GPU_LOGICOP %d", g.logic, p.logicOp);
  else std::snprintf(b, sizeof b, "mode %d src %d dst %d -> eq %d factors %d,%d%s", g.mode, g.src, g.dst, p.eqRgb, p.srcRgb, p.dstRgb,
                     p.blendsAnything ? "" : " (overwrite)");
  return b;
}

}  // namespace t2p
