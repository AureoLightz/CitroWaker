#include "model.h"

#include <algorithm>
#include <cmath>

namespace t2p {

static inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static inline float q8(float x) { return std::floor(x * 255.0f + 0.5f) / 255.0f; }
// The nudge makes values that sit exactly on a rounding boundary (k/510: a result of the 0.5 scale or of a lerp, computed through different float operations by the model and by the
// PICA program) round the same way on both sides; it is far below anything the 8 bit data distinguishes.
float snap8(float x) { return std::floor(x * 255.0f + 0.5f + 1e-4f) / 255.0f; }

// ------------------------------------------------------------------ TEV
LeafTable buildLeaves(const Rgba regs[4], const Inputs& in, const TevStage& st, bool alpha) {
  LeafTable t{};
  auto set3 = [&](Leaf l, float a, float b, float c) { t[l].v[0] = a; t[l].v[1] = b; t[l].v[2] = c; };
  auto bc = [&](Leaf l, float x) { set3(l, x, x, x); };
  const Rgba& p = regs[0];
  set3(L_CPREV, p.v[0], p.v[1], p.v[2]);
  bc(L_APREV, p.v[3]);
  for (int i = 0; i < 3; i++) {
    const Rgba& r = regs[i + 1];
    set3(Leaf(L_C0 + 2 * i), r.v[0], r.v[1], r.v[2]);
    bc(Leaf(L_A0 + 2 * i), r.v[3]);
  }
  Rgba tex{}, ras{};
  {
    const Rgba rawRas = in.ras[st.rasChan & 1];
    for (int i = 0; i < 4; i++) ras.v[i] = rawRas.v[st.rasSwap[i] & 3];
    if (st.texmap >= 0) {
      const Rgba rawTex = in.tex[st.texmap & 7];
      for (int i = 0; i < 4; i++) tex.v[i] = rawTex.v[st.texSwap[i] & 3];
    }
  }
  set3(L_TEXC, tex.v[0], tex.v[1], tex.v[2]);
  bc(L_TEXA, tex.v[3]);
  set3(L_RASC, ras.v[0], ras.v[1], ras.v[2]);
  bc(L_RASA, ras.v[3]);
  bc(L_ONE, 1.0f); bc(L_HALF, 0.5f); bc(L_ZERO, 0.0f);
  auto konst = [&](Leaf l, const KSel& k) {
    const Rgba& kc = in.kcolor[k.idx & 3];
    switch (k.kind) {
      case KSel::Fixed: bc(l, k.fixed); break;
      case KSel::Rgb: set3(l, kc.v[0], kc.v[1], kc.v[2]); break;
      case KSel::Chan: bc(l, kc.v[k.chan & 3]); break;
    }
  };
  konst(L_KONST, alpha ? st.ka : st.kc);
  konst(L_KONSTC, st.kc);
  return t;
}

bool tevCompareGT(const TevChan& c, const LeafTable& t) {
  // the hardware compares the 8 bit input values: a constant such as HALF (0.5 here) is 128, the level of a texel 128/255 -> equal, not greater
  return snap8(t[c.in[0]].v[0]) > snap8(t[c.in[1]].v[0]);
}


V3 evalTevChan(const TevChan& c, const LeafTable& t) {
  V3 r;
  if (c.cmp) {
    const bool gt = tevCompareGT(c, t);
    for (int i = 0; i < 3; i++) {
      const float x = t[c.in[3]].v[i] + (gt ? t[c.in[2]].v[i] : 0.0f);
      r.v[i] = c.clamp ? clamp01(x) : x;
    }
    return r;
  }
  for (int i = 0; i < 3; i++) {
    float a = t[c.in[0]].v[i], b = t[c.in[1]].v[i], cc = t[c.in[2]].v[i], d = t[c.in[3]].v[i];
    float l = (1 - cc) * a + cc * b;  // lerp(a,b,c)
    float x = (d + (c.sub ? -l : l) + c.bias) * c.scale;
    r.v[i] = c.clamp ? clamp01(x) : x;
  }
  return r;
}

Rgba evalTev(const TevMaterial& m, const Inputs& in) {
  Rgba regs[4];
  regs[0] = in.prev;
  for (int i = 0; i < 3; i++) regs[i + 1] = in.reg[i];
  for (const TevStage& st : m.stages) {
    V3 c = evalTevChan(st.c, buildLeaves(regs, in, st, false));
    V3 a = evalTevChan(st.a, buildLeaves(regs, in, st, true));
    for (int i = 0; i < 3; i++) regs[st.c.out & 3].v[i] = c.v[i];
    regs[st.a.out & 3].v[3] = a.v[0];
  }
  return regs[0];
}

// ------------------------------------------------------------------ PICA
V3 picaChanEval(POp op, const V3 a[3], uint8_t scale, bool quant) {
  V3 r;
  for (int i = 0; i < 3; i++) {
    float x = a[0].v[i], y = a[1].v[i], z = a[2].v[i], o = 0;
    switch (op) {
      case OP_REPLACE: o = x; break;
      case OP_MODULATE: o = x * y; break;
      case OP_ADD: o = x + y; break;
      case OP_ADDSIGNED: o = x + y - 0.5f; break;
      case OP_LERP: o = x * z + y * (1 - z); break;
      case OP_SUB: o = x - y; break;
      case OP_MAD: o = x * y + z; break;
      case OP_ADDMUL: o = std::min(x + y, 1.0f) * z; break;
      default: break;
    }
    o = clamp01(o);
    if (quant) o = q8(o);
    r.v[i] = clamp01(o * scale);
  }
  return r;
}

Rgba resolveConst(const PStage& s, const Inputs& in, bool quantize) {
  Rgba k;
  for (int i = 0; i < 4; i++) {
    const KComp& c = s.k[i];
    float v = 0;
    if (c.set) {
      if (c.sym == SYM_NONE) v = c.fixed;
      else {
        const Rgba& src = c.sym < SYM_R0 ? in.kcolor[c.sym - SYM_K0] : in.reg[c.sym - SYM_R0];
        v = src.v[c.chan];
      }
      if (c.inv) v = 1 - v;
    }
    k.v[i] = quantize ? q8(clamp01(v)) : clamp01(v);
  }
  return k;
}

static V3 applyOpr(POpr o, const Rgba& s) {
  V3 r;
  bool inv = o & 1;
  int base = o >> 1;  // 0 rgb, 1 a, 2 r, 3 g, 4 b
  for (int i = 0; i < 3; i++) {
    float x = base == 0 ? s.v[i] : s.v[base == 1 ? 3 : base - 2];
    r.v[i] = inv ? 1 - x : x;
  }
  return r;
}

Rgba evalPica(const PicaProgram& p, const Inputs& in, bool quantize, bool globalStart, const Rgba* localBuf) {
  Rgba prev = globalStart ? Rgba{} : in.prev;  // zero before stage 0 for a whole material (Citra/Azahar; hardware-unverified)
  // Combiner buffer, Azahar semantics: a stage reads combiner_buffer, then combiner_buffer = next_combiner_buffer,
  // then the stage's own update goes into next_combiner_buffer. So updates become visible two stages later.
  Rgba buf = localBuf ? *localBuf : Rgba{};
  Rgba nextBuf{};  // initial value = tev_combiner_buffer_color register (we program 0)
  for (size_t si = 0; si < p.stages.size(); si++) {
    const PStage& st = p.stages[si];
    Rgba K = resolveConst(st, in, quantize);
    auto src = [&](PSrc s) -> Rgba {
      switch (s) {
        case P_PRIMARY: return in.ras[0];
        case P_TEX0: return in.tex[0];
        case P_TEX1: return in.tex[1];
        case P_TEX2: return in.tex[2];
        case P_CONST: return K;
        case P_BUF: return buf;
        default: return prev;
      }
    };
    V3 ca[3], aa[3];
    for (int j = 0; j < 3; j++) {
      PSrc sc = st.c.arg[j].src, sa = st.a.arg[j].src;
      if (globalStart && si == 0 && j < 2) {  // stage-0 quirk: Previous in slot 0/1 reads slot 2's source
        if (sc == P_PREV) sc = st.c.arg[2].src;
        if (sa == P_PREV) sa = st.a.arg[2].src;
      }
      ca[j] = applyOpr(st.c.arg[j].opr, src(sc));
      aa[j] = applyOpr(st.a.arg[j].opr, src(sa));
      if (st.c.arg[j].snap) for (float& f : ca[j].v) f = snap8(f);
      if (st.a.arg[j].snap) for (float& f : aa[j].v) f = snap8(f);
    }
    V3 c = picaChanEval(st.c.op, ca, st.c.scale, quantize);
    V3 a = picaChanEval(st.a.op, aa, st.a.scale, quantize);
    prev = Rgba{{c.v[0], c.v[1], c.v[2], a.v[0]}};
    if (!localBuf) {
      buf = nextBuf;
      if (si < 4 && st.updBufC) for (int i = 0; i < 3; i++) nextBuf.v[i] = prev.v[i];
      if (si < 4 && st.updBufA) nextBuf.v[3] = prev.v[3];
    }
  }
  return prev;
}

uint32_t encodeBufferUpdate(const PicaProgram& p) {
  uint32_t rgb = 0, a = 0;
  for (size_t i = 0; i < p.stages.size() && i < 4; i++) {
    if (p.stages[i].updBufC) rgb |= 1u << i;
    if (p.stages[i].updBufA) a |= 1u << i;
  }
  return (rgb << 8) | (a << 12);
}

// ------------------------------------------------------------------ hardware words
const uint16_t kPicaTexEnvReg[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};

static uint32_t hwSrc(PSrc s) {
  switch (s) {
    case P_PRIMARY: return 0x0;
    case P_TEX0: return 0x3;
    case P_TEX1: return 0x4;
    case P_TEX2: return 0x5;
    case P_CONST: return 0xE;
    case P_BUF: return 0xD;
    default: return 0xF;
  }
}
static uint32_t hwOp(POp o) {
  static const uint8_t m[OP_COUNT] = {0, 1, 2, 3, 4, 5, 8, 9};  // replace..lerp, sub, mad, add-then-mul
  return m[o];
}
static uint32_t hwScale(uint8_t s) { return s == 4 ? 2 : (s == 2 ? 1 : 0); }
// libctru GPU_TEVOP_RGB_*: color operands have gaps (G=8, 1-G=9, B=12, 1-B=13); the alpha path is dense 0..7.
static uint32_t hwOprC(POpr o) {
  static const uint8_t m[10] = {0, 1, 2, 3, 4, 5, 8, 9, 12, 13};  // RGB,1RGB,A,1A,R,1R,G,1G,B,1B
  return m[o];
}

HwStage encodeStage(const PStage& s, const Rgba& k) {
  HwStage h{};
  for (int j = 0; j < 3; j++) {
    h.sources |= hwSrc(s.c.arg[j].src) << (4 * j);
    h.sources |= hwSrc(s.a.arg[j].src) << (16 + 4 * j);
    h.modifiers |= hwOprC(s.c.arg[j].opr) << (4 * j);                // color operand codes have gaps, see hwOprC
    // alpha: A,1A,R,1R,G,1G,B,1B -> 0..7 (an unused slot may still hold a color operand; encode it as plain alpha)
    h.modifiers |= uint32_t(s.a.arg[j].opr >= OPR_A ? s.a.arg[j].opr - OPR_A : 0) << (12 + 4 * j);
  }
  h.ops = hwOp(s.c.op) | (hwOp(s.a.op) << 16);
  auto b = [](float x) { return uint32_t(std::floor(x * 255.0f + 0.5f)); };
  h.constColor = b(k.v[0]) | (b(k.v[1]) << 8) | (b(k.v[2]) << 16) | (b(k.v[3]) << 24);
  h.scales = hwScale(s.c.scale) | (hwScale(s.a.scale) << 16);
  return h;
}

}  // namespace t2p
