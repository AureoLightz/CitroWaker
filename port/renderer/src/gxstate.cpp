#include <optional>
#include "gxstate.h"
#include "texfit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

thread_local uint64_t g_t2pStateProf[8];   // (Claude v100) state decode sections, read by the [prof-state] log line
namespace t2p {
bool g_uniformSections=false;
bool g_uniformPieces=false;
bool g_compactPoseStorage=false;
std::shared_ptr<const CompactUniforms> compactUniforms(const VtxUniforms& u,int npose){
 static thread_local std::shared_ptr<const CompactUniforms> last;
 // A snapshot may advertise only rows whose current values were checked.
 // Reusing a larger prefix after unused XF rows change can otherwise expose
 // stale matrices when a later draw increases its pose count without reloading.
 // (Claude v112) only the pose rows a draw with `npose` position matrices reads are compared and kept; the block comes from a pool
 if(last&&last->npose==npose&&last->matches(u,npose))return last;
 const UniformNonPose wanted(u);
 static thread_local std::weak_ptr<const UniformNonPose> entries[64];
 static thread_local uint32_t hashes[64]={};static thread_local unsigned next=0;
 std::shared_ptr<const UniformNonPose> part;
 if(last&&!std::memcmp(last->shared.get(),&wanted,sizeof wanted))part=last->shared;
 if(!part){
  // (Claude v112) a word at a time (was a byte at a time: 1112 dependent multiplies per new pose block on the ARM11); a bucket hash only, every hit is memcmp-confirmed
  static_assert(sizeof wanted%4==0,"word rows");
  uint32_t hash=2166136261u;const auto* p=reinterpret_cast<const uint32_t*>(&wanted);
  for(size_t k=0;k<sizeof wanted/4;++k)hash=(hash^p[k])*16777619u;
  for(unsigned k=0;k<64;++k)if(hashes[k]==hash){auto candidate=entries[k].lock();if(candidate&&!std::memcmp(candidate.get(),&wanted,sizeof wanted)){part=std::move(candidate);break;}}
  if(!part){part=std::make_shared<const UniformNonPose>(wanted);unsigned k=next++&63;hashes[k]=hash;entries[k]=part;}
 }
 last=makeCompactUniforms(u,npose,std::move(part));return last;
}

// decodeState(..., notes=false): the diagnostic GxState::notes are not built (they are only copied into
// packets when RendererOptions::packetNotes is on; nothing else reads them). Set per decodeState call.
thread_local bool tl_notes = true;

namespace {

inline uint32_t bp(const GxRegs& r, int id) { return r.bp[id] & 0xFFFFFF; }
inline float f32(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
inline float xff(const GxRegs& r, uint32_t addr) { return f32(r.xf[addr]); }
inline int sext(uint32_t v, int bits) { const uint32_t m = 1u << (bits - 1); v &= (1u << bits) - 1; return int(v ^ m) - int(m); }
std::string num(int v) { return std::to_string(v); }

// ---------------------------------------------------------------- TEV
const Leaf kColorLeaf[16] = {L_CPREV, L_APREV, L_C0, L_A0, L_C1, L_A1, L_C2, L_A2, L_TEXC, L_TEXA, L_RASC, L_RASA, L_ONE, L_HALF, L_KONST, L_ZERO};
const Leaf kAlphaLeaf[8] = {L_APREV, L_A0, L_A1, L_A2, L_TEXA, L_RASA, L_KONST, L_ZERO};
const float kScale[4] = {1.0f, 2.0f, 4.0f, 0.5f};

KSel decodeKSel(int sel, bool alpha, bool& valid) {
  KSel k;
  valid = true;
  if (sel < 8) { k.kind = KSel::Fixed; k.fixed = float(8 - sel) / 8.0f; }
  else if (!alpha && sel >= 12 && sel <= 15) { k.kind = KSel::Rgb; k.idx = uint8_t(sel - 12); }
  else if (sel >= 16) { k.kind = KSel::Chan; k.idx = uint8_t(sel & 3); k.chan = uint8_t((sel - 16) >> 2); }
  else valid = false;
  return k;
}

void decodeTev(const GxRegs& r, GxState& S, const GxDraw*) {
  GxDrawState& D = S.draw;
  // swap tables: table t = regs 0xF6+2t (red, green) and 0xF6+2t+1 (blue, alpha), each 2 bits
  uint8_t swapTab[4][4];
  for (int t = 0; t < 4; t++) {
    const uint32_t a = bp(r, 0xF6 + 2 * t), b = bp(r, 0xF6 + 2 * t + 1);
    swapTab[t][0] = uint8_t(a & 3); swapTab[t][1] = uint8_t((a >> 2) & 3);
    swapTab[t][2] = uint8_t(b & 3); swapTab[t][3] = uint8_t((b >> 2) & 3);
  }
  D.tev.stages.clear();
  for (int s = 0; s < S.numTevStages; s++) {
    TevStage st;
    const uint32_t cv = bp(r, 0xC0 + 2 * s), av = bp(r, 0xC1 + 2 * s);
    const uint32_t tref = bp(r, 0x28 + s / 2), ks = bp(r, 0xF6 + s / 2);
    // order
    int texmap, texcoord, enable, chan;
    if (s & 1) { texmap = (tref >> 12) & 7; texcoord = (tref >> 15) & 7; enable = (tref >> 18) & 1; chan = (tref >> 19) & 7; }
    else { texmap = tref & 7; texcoord = (tref >> 3) & 7; enable = (tref >> 6) & 1; chan = (tref >> 7) & 7; }
    st.texmap = enable ? texmap : -1;
    st.texcoord = enable ? texcoord : -1;
    // colour / alpha equations
    TevChan& c = st.c;
    c.in[3] = kColorLeaf[cv & 15]; c.in[2] = kColorLeaf[(cv >> 4) & 15]; c.in[1] = kColorLeaf[(cv >> 8) & 15]; c.in[0] = kColorLeaf[(cv >> 12) & 15];
    TevChan& a = st.a;
    a.in[3] = kAlphaLeaf[(av >> 4) & 7]; a.in[2] = kAlphaLeaf[(av >> 7) & 7]; a.in[1] = kAlphaLeaf[(av >> 10) & 7]; a.in[0] = kAlphaLeaf[(av >> 13) & 7];
    for (int ch = 0; ch < 2; ch++) {
      const uint32_t v = ch == 0 ? cv : av;
      TevChan& t = ch == 0 ? c : a;
      const int bias = (v >> 16) & 3;
      t.sub = (v >> 18) & 1;
      t.clamp = (v >> 19) & 1;
      t.scale = kScale[(v >> 20) & 3];
      t.out = uint8_t((v >> 22) & 3);
      if (bias == 3) {
        // compare mode: bit 18 = EQ (not GT), bits 20..21 = what is compared (colour: R8 GR16 BGR24 RGB8, alpha: R8 GR16 BGR24 A8). Modelled: R8_GT (both channels), A8_GT (alpha).
        const int kind = (v >> 20) & 3, eq = (v >> 18) & 1;
        static const char* kKinds[2][4] = {{"R8", "GR16", "BGR24", "RGB8"}, {"R8", "GR16", "BGR24", "A8"}};
        if (!eq && kind == 0) t.cmp = 1;
        else if (!eq && kind == 3 && ch == 1) t.cmp = 2;
        else D.importBlockers.push_back("stage " + num(s) + ": TEV " + (ch == 0 ? "colour" : "alpha") + " compare (" + kKinds[ch][kind] + (eq ? " ==" : " >") + "), not modelled");
        t.bias = 0; t.scale = 1; t.sub = false;
      } else {
        t.bias = bias == 1 ? 0.5f : (bias == 2 ? -0.5f : 0.0f);
      }
    }
    if (a.cmp == 1) setAlphaR8Operands(st);   // the alpha R8 compare reads the colour inputs a and b
    // konstant selection
    bool kcOk, kaOk;
    const int kcsel = (s & 1) ? int((ks >> 14) & 31) : int((ks >> 4) & 31);
    const int kasel = (s & 1) ? int((ks >> 19) & 31) : int((ks >> 9) & 31);
    st.kc = decodeKSel(kcsel, false, kcOk);
    st.ka = decodeKSel(kasel, true, kaOk);
    bool readsK = false, readsRas = false;
    for (Leaf l : c.in) { if (l == L_KONST) readsK = true; if (l == L_RASC || l == L_RASA) readsRas = true; }
    bool readsKa = false;
    for (Leaf l : a.in) { if (l == L_KONST) readsKa = true; if (l == L_KONSTC) readsK = true; if (l == L_RASA || l == L_RASC) readsRas = true; }
    if (!kcOk && readsK) D.importBlockers.push_back("stage " + num(s) + ": invalid konstant colour selection " + num(kcsel));
    if (!kaOk && readsKa) D.importBlockers.push_back("stage " + num(s) + ": invalid konstant alpha selection " + num(kasel));
    // raster channel
    if (chan == 0 || chan == 1) st.rasChan = chan;
    else {
      st.rasChan = 0;
      if (readsRas) D.importBlockers.push_back("stage " + num(s) + ": reads the raster colour but its channel is " + (chan == 7 ? "COLOR_NULL (zero)" : "alpha bump") + ", not modelled");
    }
    // swaps
    const int rasSel = av & 3, texSel = (av >> 2) & 3;
    for (int i = 0; i < 4; i++) { st.rasSwap[i] = swapTab[rasSel][i]; st.texSwap[i] = swapTab[texSel][i]; }
    if (bp(r, 0x10 + s)) D.indirect = true;
    if (s < 3) D.indRaw[s] = bp(r, 0x10 + s);
    if (s == 0) D.indRaw[3] = bp(r, 0x27) | (((r.bp[0] >> 16) & 7u) << 24);
    D.tev.stages.push_back(st);
  }
}

// ---------------------------------------------------------------- vertex channels, texgens
VtxChan decodeChan(uint32_t v) {
  VtxChan c;
  c.matSrc = v & 1;
  c.enable = (v >> 1) & 1;
  c.lightMask = uint8_t(((v >> 2) & 0xF) | (((v >> 11) & 0xF) << 4));
  c.ambSrc = (v >> 6) & 1;
  c.diffuse = uint8_t((v >> 7) & 3);
  const bool attnEnable = (v >> 9) & 1, attnSelect = (v >> 10) & 1;
  c.attn = !attnEnable ? 2 : (attnSelect ? 1 : 0);
  return c;
}

void decodeTexGens(const GxRegs& r, GxState& S, const GxVcd& vcd) {
  GxDrawState& D = S.draw;
  D.vtx.texgen.clear();
  const bool dual = r.xf[0x1012] & 1;
  for (int i = 0; i < S.numTexGens && i < 8; i++) {
    const uint32_t v = r.xf[0x1040 + i], d = r.xf[0x1050 + i];
    VtxTexGen g;
    const int kind = (v >> 4) & 7, row = (v >> 7) & 31;
    const bool proj3 = (v >> 1) & 1;
    if (kind == 0) {
      g.type = proj3 ? 0 : 1;
      if (row == 0) g.src = 0; else if (row == 1) g.src = 1; else if (row == 2) g.src = 19;
      else if (row == 3) g.src = 2; else if (row == 4) g.src = 3; else if (row >= 5 && row <= 12) g.src = uint8_t(4 + row - 5);
      else { if (tl_notes) S.notes.push_back("texgen " + num(i) + ": unknown source row " + num(row)); }
    } else if (kind == 1) {
      g.type = 2;   // emboss / bump: flagged by the generator
      g.src = uint8_t(row >= 5 && row <= 12 ? 4 + row - 5 : 0);
    } else {
      g.type = 10;  // SRTG
      g.src = kind == 2 ? 19 : 20;
    }
    int mtxNumber;
    if (i < 4) mtxNumber = int((r.xf[0x1018] >> (6 + 6 * i)) & 63);
    else mtxNumber = int((r.xf[0x1019] >> (6 * (i - 4))) & 63);
    g.mtx = uint8_t(mtxNumber);
    if (dual) { g.normalize = (d >> 8) & 1; g.postMtx = uint8_t(((d & 63) + 64)); }
    else { g.normalize = false; g.postMtx = 125; }
    D.vtx.texgen.push_back(g);
    // attribute availability (the hardware reads zeros for attributes the stream does not carry)
    if (g.type <= 1 && g.src >= 4 && g.src <= 11 && vcd.tex[g.src - 4] == GXA_NONE)
      { if (tl_notes) S.notes.push_back("texgen " + num(i) + " reads TEX" + num(g.src - 4) + " but the vertex descriptor has no such attribute"); }
    if (g.type <= 1 && g.src == 1 && vcd.nrm == GXA_NONE)
      { if (tl_notes) S.notes.push_back("texgen " + num(i) + " reads the normal but the vertex descriptor has none"); }
  }
}

// ---------------------------------------------------------------- fog
float compactFloat(uint32_t v) {   // 1 sign, 8 exponent, 11 mantissa bits (BP fog A and C)
  const uint32_t bits = (((v >> 19) & 1) << 31) | (((v >> 11) & 0xFF) << 23) | ((v & 0x7FF) << 12);
  return f32(bits);
}

void decodeFog(const GxRegs& r, GxState& S) {
  GxFog& F = S.draw.fog;
  F = GxFog();
  const uint32_t p0 = bp(r, 0xEE), p1 = bp(r, 0xEF), p2 = bp(r, 0xF0), p3 = bp(r, 0xF1), col = bp(r, 0xF2);
  const int type = (p3 >> 21) & 7, ortho = (p3 >> 20) & 1;
  if (type == 0) return;
  const float a = compactFloat(p0), c = compactFloat(p3);
  const int bs = int(p2 & 31);
  const double A = double(a) * std::ldexp(1.0, bs);                 // ze = A / (B - zs) - C  (zs = screen depth in [0,1])
  const double B = double(p1 & 0xFFFFFF) * std::ldexp(1.0, bs - 24);
  F.r = uint8_t(col >> 16); F.g = uint8_t(col >> 8); F.b = uint8_t(col);
  if (A == 0.0 || B <= 0.0) { { if (tl_notes) S.notes.push_back("fog configured with a degenerate range: treated as off"); } return; }
  if (ortho || S.proj.ortho) { { if (tl_notes) S.notes.push_back("orthographic fog is not translated: fog off"); } return; }
  const double n = S.proj.nearZ, f = S.proj.farZ;
  if (!(n > 0 && f > n)) { { if (tl_notes) S.notes.push_back("fog without a perspective projection: fog off"); } return; }
  // With the fog planes equal to the projection planes (what the game passes), B = f/(f-n) and A = n*B/(end-start).
  // The SDK's B carries a +3.6e-6 relative bias and the 24 bit mantissa rounds at 1.2e-7, so 1e-5 separates "same planes" from
  // different ones (B - 1 = n / (f - n) is tiny for wide ranges, hence the tight tolerance).
  double fn = n;
  const double Bexp = f / (f - n);
  if (std::fabs(B / Bexp - 1.0) > 1e-5) {
    fn = f * (B - 1.0) / B;
    { if (tl_notes) S.notes.push_back("fog planes differ from the projection planes: fog ramp is approximate"); }
    if (!(fn > 0)) { { if (tl_notes) S.notes.push_back("fog planes unusable: fog off"); } return; }
  }
  const double span = fn * B / A, start = double(c) * span;
  F.type = uint8_t(type);
  F.startZ = float(start); F.endZ = float(start + span);
  F.nearZ = float(n); F.farZ = float(f);
}

// ---------------------------------------------------------------- projection / viewport / scissor
void decodeProjection(const GxRegs& r, GxProjection& P) {
  for (int i = 0; i < 6; i++) P.p[i] = xff(r, 0x1020 + i);
  P.ortho = r.xf[0x1026] != 0;
  std::memset(P.m, 0, sizeof P.m);
  P.m[0][0] = P.p[0]; P.m[1][1] = P.p[2]; P.m[2][2] = P.p[4]; P.m[2][3] = P.p[5];
  if (P.ortho) { P.m[0][3] = P.p[1]; P.m[1][3] = P.p[3]; P.m[3][3] = 1; }
  else { P.m[0][2] = P.p[1]; P.m[1][2] = P.p[3]; P.m[3][2] = -1; }
  P.nearZ = P.farZ = 0;
  if (P.p[4] != 0) {
    P.farZ = P.p[5] / P.p[4];
    if (P.ortho) P.nearZ = P.farZ + 1.0f / P.p[4];
    else if (P.p[4] != 1) P.nearZ = P.p[5] / (P.p[4] - 1.0f);
  }
}

}  // namespace

int GxTexBinding::mipLevels() const {
  if (mipMode == 0 || width <= 0 || height <= 0) return 1;
  int top = 0;
  for (int m = std::max(width, height); m > 1; m >>= 1) top++;
  return std::min(top, int(std::floor(maxLod))) + 1;
}

namespace {
bool g_stateMemo = true;
// TEV: decodeTev reads GEN_MODE's stage count and BP 0x10..0x1F (indirect), 0x28..0x2F (TREF), 0xC0..0xDF (combiners), 0xF6..0xFD (swap / ksel); it writes
// D.tev.stages, appends D.importBlockers (empty before it: the state is fresh) and may set D.indirect.
struct TevMemo { uint32_t hash=0; bool valid = false; uint32_t key[65]; MaterialVector<TevStage> stages; std::vector<std::string> blockers; bool indirect = false; uint32_t indRaw[4] = {}; };
// texgens (only without notes: with notes decodeTexGens also reads the vertex descriptor): XF 0x1012, 0x1018, 0x1019, 0x1040..0x1047, 0x1050..0x1057, numTexGens.
struct TexGenMemo { uint32_t hash=0; bool valid = false; uint32_t key[20]; MaterialVector<VtxTexGen> texgen; };
// one texmap: its five BP words, whether TX_SETIMAGE0 was written and the Aurora address -> the binding and its texmapScaled bit.
struct TexMemo { bool valid = false; uint32_t key[7]; GxTexBinding t; bool scaled = false; };
thread_local TevMemo t_tev;
thread_local TexGenMemo t_texgen;
thread_local TexMemo t_tex[8];
}  // namespace

void setStateDecodeMemo(bool on) { g_stateMemo = on; }

static void decodeStateSections(GxState& S, const GxRegs& r, const GxDraw* draw, bool notes, int knownMaxPm, uint32_t dirty, const GxState* prev);

GxState decodeState(const GxRegs& r, const GxDraw* draw, bool notes, int knownMaxPm) {
  GxState S;
  decodeStateInto(S, r, draw, notes, knownMaxPm);
  return S;
}

void decodeStateInto(GxState& S, const GxRegs& r, const GxDraw* draw, bool notes, int knownMaxPm) {
  decodeStateSections(S, r, draw, notes, knownMaxPm, kStateDirtyAll, nullptr);
}

// (Claude v79) equality of two memo keys (whole words), inline: same result as memcmp(...) == 0
template <size_t N> static inline bool sameKey(const uint32_t (&a)[N], const uint32_t (&b)[N]) {
  uint32_t d = 0;
  for (size_t i = 0; i < N; i++) d |= a[i] ^ b[i];
  return d == 0;
}
// A bounded exact material memo retains immutable decoded rows across intervening
// materials, rather than just the immediately preceding material. Hashes select
// a four-way set only; full register keys establish every hit.
template<class Memo,size_t N,size_t Sets>
static Memo& materialMemo(const uint32_t (&key)[N]){
 struct Cache {Memo entries[Sets][4];uint8_t next[Sets]={};};
 static_assert(Sets&&(Sets&(Sets-1))==0);
 static thread_local std::unique_ptr<Cache> owner;
 if(!owner)owner=std::make_unique<Cache>();
 Cache& cache=*owner;
 uint32_t h=2166136261u;for(auto v:key)h=(h^v)*16777619u;
 const unsigned bucket=h&(Sets-1);
 for(auto& m:cache.entries[bucket])if(m.valid&&m.hash==h&&sameKey(m.key,key))return m;
 Memo* victim=nullptr;for(auto& m:cache.entries[bucket])if(!m.valid){victim=&m;break;}
 if(!victim)victim=&cache.entries[bucket][cache.next[bucket]++&3];
 victim->valid=false;victim->hash=h;return *victim;
}
// (Claude v79) bit equality of two uniform blocks, word by word (newlib's memcmp may go byte by byte; neighbouring blocks share long equal prefixes)
static bool sameUniformWords(const VtxUniforms& a, const VtxUniforms& b) {
  static_assert(sizeof(VtxUniforms) % 16 == 0 || sizeof(VtxUniforms) % 4 == 0, "whole words");
  const uint32_t* x = reinterpret_cast<const uint32_t*>(&a); const uint32_t* y = reinterpret_cast<const uint32_t*>(&b);
  constexpr size_t n = sizeof(VtxUniforms) / 4;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) if ((x[i] ^ y[i]) | (x[i+1] ^ y[i+1]) | (x[i+2] ^ y[i+2]) | (x[i+3] ^ y[i+3])) return false;
  for (; i < n; i++) if (x[i] != y[i]) return false;
  return true;
}

void decodeStateIncremental(GxState& S, const GxRegs& r, const GxDraw* draw, bool notes, int knownMaxPm, uint32_t dirty, const GxState* prev) {
  if (!prev || notes || dirty == kStateDirtyAll || (dirty & kStateDirtyOther)) { decodeStateSections(S, r, draw, notes, knownMaxPm, kStateDirtyAll, nullptr); return; }
  // sections that depend on another section's registers
  if (dirty & kStateDirtyGen) dirty |= kStateDirtyTev | kStateDirtyTexGen;   // stage count, texgen count
  if (dirty & kStateDirtyProj) dirty |= kStateDirtyFog;                      // the fog ramp reads the projection planes
  const uint64_t c0 = t2pProfTicks();
  S = *prev;
  if (t2pProfEnabled()) { g_t2pStateProf[5] += t2pProfTicks() - c0; g_t2pProf[14]++; }   // (Claude v100) slot 5: the copy of the previous state
  decodeStateSections(S, r, draw, false, knownMaxPm, dirty, prev);
}

void decodeStateTemplate(GxState& S,const GxRegs& r,const GxDraw* draw,int knownMaxPm,uint32_t dirty,const GxState* prev,const GxState& material) {
  S=material;
  constexpr uint32_t dynamic=kStateDirtyXfMtx|kStateDirtyLight|kStateDirtyChan|kStateDirtyProj|kStateDirtyFog;
  if(prev){
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
    S.uniformBlock=prev->uniformBlock;S.uniformSections=prev->uniformSections;
#else
    S.uniforms=prev->uniforms;
#endif
    S.uniformPosSel=prev->uniformPosSel;S.proj=prev->proj;
  }
  // Unknown invalidation or no live predecessor gives no clean-uniform proof.
  uint32_t refresh=(!prev||dirty==kStateDirtyAll||(dirty&kStateDirtyOther))?dynamic:dirty&dynamic;
  if(refresh&kStateDirtyProj)refresh|=kStateDirtyFog;
  decodeStateSections(S,r,draw,false,knownMaxPm,refresh,prev);
}

// `dirty` selects the sections decoded (all of them: kStateDirtyAll, S fresh); with prev, S is a copy of *prev and each decoded section first puts
// back what a fresh state has where the section accumulates (importBlockers, indirect, texmapScaled, projFar).
static void decodeStateSections(GxState& S, const GxRegs& r, const GxDraw* draw, bool notes, int knownMaxPm, uint32_t dirty, const GxState* prev) {
  tl_notes = notes;
  GxDrawState& D = S.draw;
  const GxVcd vcd = draw ? draw->vcd : r.vcd();
  const bool all = dirty == kStateDirtyAll;

  // ---- GEN_MODE
  if (dirty & kStateDirtyGen) {
  const uint32_t gm = bp(r, 0x00);
  S.numTexGens = int(gm & 15);
  S.numChans = int((gm >> 4) & 7);
  S.numTevStages = int((gm >> 10) & 15) + 1;
  { const int c = int((gm >> 14) & 3); S.cullMode = uint8_t(c == 1 ? 2 : (c == 2 ? 1 : c)); }
  S.numIndStages = int((gm >> 16) & 7);
  }

  // ---- projection first (fog needs it)
  if (dirty & kStateDirtyProj) {
  if (!all) D.projFar = GxDrawState().projFar;
  decodeProjection(r, S.proj);
  if (S.proj.farZ > 0) D.projFar = S.proj.farZ;
  D.projOrtho = S.proj.ortho;
  }

  uint64_t sq = t2pProfTicks();
  auto smark = [&](int k) { if (t2pProfEnabled()) { const uint64_t t = t2pProfTicks(); g_t2pStateProf[k - 9] += t - sq; sq = t; } };   // (Claude v100) own slots: 0 TEV, 1 vertex pipeline, 2 uniforms, 3 pixel+viewport, 4 textures
  // ---- TEV, constants
  if (dirty & kStateDirtyTev) {
    if (!all) { D.importBlockers.clear(); D.indirect = false; }
    uint32_t key[65]; int k = 0;
    key[k++] = uint32_t(S.numTevStages);
    for (int i = 0x10; i < 0x20; i++) key[k++] = bp(r, i);
    for (int i = 0x28; i < 0x30; i++) key[k++] = bp(r, i);
    for (int i = 0xC0; i < 0xE0; i++) key[k++] = bp(r, i);
    for (int i = 0xF6; i < 0xFE; i++) key[k++] = bp(r, i);
    TevMemo& m = g_sharedMaterialVectors&&g_stateMemo?materialMemo<TevMemo,65,32>(key):t_tev;
    if (g_stateMemo && m.valid && (g_sharedMaterialVectors||sameKey(m.key, key))) {
      D.tev.stages = m.stages; D.importBlockers = m.blockers; if (m.indirect) D.indirect = true; std::memcpy(D.indRaw, m.indRaw, sizeof m.indRaw);
    } else {
      decodeTev(r, S, draw);
      freezeMaterialVector(D.tev.stages);
      if (g_stateMemo) { std::memcpy(m.key, key, sizeof key); m.stages = D.tev.stages; m.blockers = D.importBlockers; m.indirect = D.indirect; std::memcpy(m.indRaw, D.indRaw, sizeof m.indRaw); m.valid = true; }
    }
  }
  if (dirty & kStateDirtyTevReg)
  for (int i = 0; i < 4; i++) {
    const uint32_t ra = r.tevRaw[0][i * 2], bg = r.tevRaw[0][i * 2 + 1];
    S.tevReg[i].v[0] = float(sext(ra, 11)) / 255.0f;
    S.tevReg[i].v[3] = float(sext(ra >> 12, 11)) / 255.0f;
    S.tevReg[i].v[2] = float(sext(bg, 11)) / 255.0f;
    S.tevReg[i].v[1] = float(sext(bg >> 12, 11)) / 255.0f;
    const uint32_t kra = r.tevRaw[1][i * 2], kbg = r.tevRaw[1][i * 2 + 1];
    S.kcolor[i].v[0] = float(kra & 0xFF) / 255.0f;
    S.kcolor[i].v[3] = float((kra >> 12) & 0xFF) / 255.0f;
    S.kcolor[i].v[2] = float(kbg & 0xFF) / 255.0f;
    S.kcolor[i].v[1] = float((kbg >> 12) & 0xFF) / 255.0f;
  }

  smark(9);
  // ---- vertex pipeline
  if (dirty & (kStateDirtyChan | kStateDirtyTexGen)) {
  { auto keep = std::move(D.vtx.texgen); keep.clear(); D.vtx = VtxSetup(); D.vtx.texgen = std::move(keep); }   // (Claude v79) a fresh setup, keeping the texgen vector's storage
  D.vtx.color0 = decodeChan(r.xf[0x100E]);
  D.vtx.alpha0 = decodeChan(r.xf[0x1010]);
  if (g_stateMemo && !tl_notes) {
    uint32_t key[20]; int k = 0;
    key[k++] = uint32_t(S.numTexGens); key[k++] = r.xf[0x1012]; key[k++] = r.xf[0x1018]; key[k++] = r.xf[0x1019];
    for (int i = 0; i < 8; i++) key[k++] = r.xf[0x1040 + i];
    for (int i = 0; i < 8; i++) key[k++] = r.xf[0x1050 + i];
    TexGenMemo& m = g_sharedMaterialVectors?materialMemo<TexGenMemo,20,16>(key):t_texgen;
    if (m.valid && (g_sharedMaterialVectors||sameKey(m.key, key))) D.vtx.texgen = m.texgen;
    else { decodeTexGens(r, S, vcd); freezeMaterialVector(D.vtx.texgen); std::memcpy(m.key, key, sizeof key); m.texgen = D.vtx.texgen; m.valid = true; }
  } else {
    decodeTexGens(r, S, vcd); freezeMaterialVector(D.vtx.texgen);
  }
  }
  S.curPosMtx = int(r.xf[0x1018] & 63) / 3;
  D.numPosMtx = 1;
  const bool known = knownMaxPm != kScanPosMtx;
  const bool perVertexMtx = draw && vcd.pmIdx && (known ? draw->v.count > 0 : !draw->v.pmIdx.empty());
  if (perVertexMtx) {
    int mx = 0;
    if (known) mx = std::max(0, knownMaxPm);
    else for (uint8_t i : draw->v.pmIdx) mx = std::max<int>(mx, i / 3);
    if (mx > 9) { { if (tl_notes) S.notes.push_back("position matrix index " + num(mx) + " beyond the 10 the vertex shader supports"); } mx = 9; }
    D.numPosMtx = mx + 1;
  }
  for (int i = 0; i < 8; i++)
    if (vcd.texMtxIdx[i]) { if (tl_notes) S.notes.push_back("per-vertex texture matrix index (TEX" + num(i) + "MIDX) is not supported: the texgen's matrix is used"); }
  {
    bool litRead = D.vtx.color0.enable || D.vtx.alpha0.enable;
    if (litRead && vcd.nrm == GXA_NONE) { if (tl_notes) S.notes.push_back("lighting enabled but the vertex descriptor has no normal"); }
  }
  if (S.numChans == 0) { if (tl_notes) S.notes.push_back("no colour channel is generated (XF numChans = 0): the raster colour is not defined"); }

  smark(10);
  // ---- uniforms: a function of the projection, the matrix memory, the lights, the two colour registers and which matrix goes to slot 0
  const int posSel = (!perVertexMtx && S.curPosMtx != 0 && S.curPosMtx < 10) ? S.curPosMtx : 0;
  bool uniformsKept = prev && !(dirty & (kStateDirtyProj | kStateDirtyXfMtx | kStateDirtyLight | kStateDirtyChan)) && prev->uniformPosSel == posSel;
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
  if (uniformsKept && !prev->uniformBlock && prev->uniformSections && prev->uniformSections->npose < D.numPosMtx) uniformsKept = false;   // (Claude v112) the kept block lacks rows
#endif
  S.uniformPosSel = posSel;
  if (!uniformsKept) {
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
    // Reuse proven clean non-pose rows without rebuilding a flat 2 KiB block.
    // XF dirty groups include texture matrices: compare those bytes explicitly.
    if(g_uniformSections&&g_uniformPieces){
      const auto old=prev&&!prev->uniformBlock?prev->uniformSections:nullptr;
      auto parts=makeUniformPieces(r.xf,S.proj.m,dirty,old?old->pieces:nullptr);
      if(old&&old->npose==D.numPosMtx&&old->pieces==parts&&old->samePose(r.xf,posSel,D.numPosMtx))S.uniformSections=old;
      else S.uniformSections=makeCompactPieceUniforms(r.xf,posSel,D.numPosMtx,std::move(parts));
      S.uniformBlock.reset();
    }else if(g_uniformSections && prev && !prev->uniformBlock && prev->uniformSections &&
       !(dirty & (kStateDirtyProj | kStateDirtyLight | kStateDirtyChan)) &&
       !std::memcmp(prev->uniformSections->shared->texMtx,&r.xf[120],sizeof prev->uniformSections->shared->texMtx)) {
      // (Claude v112) only the rows the draw reads (numPosMtx; was all 10 position + 10 normal matrices, ~1 KB per GX draw), and no new block at
      // all when the previous one already holds them (J3D reloads the same joint matrix for each material of a shape)
      const CompactUniforms& pu=*prev->uniformSections;
      if(pu.npose==D.numPosMtx&&pu.samePose(r.xf,posSel,D.numPosMtx))S.uniformSections=prev->uniformSections;
      else S.uniformSections=makeCompactUniforms(r.xf,posSel,D.numPosMtx,pu.shared);
      S.uniformBlock.reset();
    } else {
#endif
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
    // (Claude v79) built straight into a new shared block (was: a stack block copied into make_shared, 2 KB per batch); dropped again when it equals the last
    // (Claude v79) base: the previous state's block when the projection, the colour registers and the lights are unchanged since it was made (each block
    // holds what its state's registers give: built from them, or kept / shared only while they were clean): its proj, matColor, ambColor, lights and
    // texScale are what the rebuild below would write, so only the matrix memory (pos, nrm, texMtx and the slot-0 selection) is made again.
    const VtxUniforms* base = (!g_uniformSections && prev && prev->uniformBlock && !(dirty & (kStateDirtyProj | kStateDirtyLight | kStateDirtyChan))) ? prev->uniformBlock.get() : nullptr;
    // (Claude v102) built in a per-producer scratch block, compared with the last published block, and only copied into a new pooled block when it
    // differs (was: a pooled block made, filled and dropped again when equal). After every build the scratch holds the last block's words, so
    // a base that is the last block needs no copy.
    static thread_local VtxUniforms scratchU;
    static thread_local std::shared_ptr<const VtxUniforms> last;
    static thread_local bool scratchIsLast = false;
    std::optional<VtxUniforms> local;
    if (g_uniformSections) local.emplace();
    else if (!base) scratchU = VtxUniforms();
    else if (!(scratchIsLast && last && base == last.get())) scratchU = *base;
    VtxUniforms& U = g_uniformSections ? *local : scratchU;
#else
    VtxUniforms& U = S.uniforms;
    constexpr const VtxUniforms* base = nullptr;
#endif
    if (!base) for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) U.proj[i][j] = S.proj.m[i][j];
    // XF words are the floats' bits: whole matrix blocks are copied as they are (pos: XF 0..119, texMtx: 120..239, both [10][3][4] in XF order)
    static_assert(sizeof U.pos == 120 * 4 && sizeof U.texMtx == 120 * 4, "matrix block layout");
    std::memcpy(U.pos, &r.xf[0], sizeof U.pos);
    std::memcpy(U.texMtx, &r.xf[120], sizeof U.texMtx);
    {   // (Claude v79) word copies (were 30 memcpy calls of 12 bytes); the XF words are the floats' bits, as before
      static_assert(sizeof(float) == sizeof(uint32_t), "float words");
      uint32_t* nw = reinterpret_cast<uint32_t*>(&U.nrm[0][0][0]);
      const uint32_t* xw = &r.xf[0x400];
      for (int row = 0; row < 30; row++, nw += 4, xw += 3) { nw[0] = xw[0]; nw[1] = xw[1]; nw[2] = xw[2]; nw[3] = 0; }
    }
    if (!perVertexMtx && S.curPosMtx != 0 && S.curPosMtx < 10) {   // no per-vertex index: the current matrix is the one the draw uses
      std::memcpy(U.pos[0], U.pos[S.curPosMtx], sizeof U.pos[0]);
      std::memcpy(U.nrm[0], U.nrm[S.curPosMtx], sizeof U.nrm[0]);
    }
    auto unpack = [](uint32_t v, float out[4]) { out[0] = float(v >> 24); out[1] = float((v >> 16) & 255); out[2] = float((v >> 8) & 255); out[3] = float(v & 255); };
    if (!base) {
    unpack(r.xf[0x100C], U.matColor);
    unpack(r.xf[0x100A], U.ambColor);
    for (int l = 0; l < 8; l++) {
      VtxLight& L = U.light[l];
      const uint32_t b = 0x600 + 16 * l;
      unpack(r.xf[b + 3], L.color);
      std::memcpy(L.cosatt, &r.xf[b + 4], 12);
      std::memcpy(L.distatt, &r.xf[b + 7], 12);
      std::memcpy(L.pos, &r.xf[b + 10], 12);
      std::memcpy(L.dir, &r.xf[b + 13], 12);
    }
    }
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
    // A bounded per-producer memo owns only one extra block. Never mutate a
    // block after publication: CPU2 can still be consuming an older frame.
    if(g_uniformSections){S.uniformSections=compactUniforms(U,D.numPosMtx);S.uniformBlock.reset();}
    else {
    if (last && sameUniformWords(*last, U)) S.uniformBlock=last;
    else { S.uniformBlock=makeUniformBlock(U);last=S.uniformBlock; }   // (Claude v86) pooled
    scratchIsLast=true;
      S.uniformSections.reset();
    }
#endif
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
    }
#endif
  }

  smark(11);
  // ---- fixed-function pixel state
  if (dirty & kStateDirtyPixel) {
    const uint32_t cm = bp(r, 0x41), zm = bp(r, 0x40), ac = bp(r, 0xF3), c1 = bp(r, 0x42), pe = bp(r, 0x43);
    GxBlend& B = D.blend;
    B.mode = (cm >> 11) & 1 ? 3 : ((cm >> 1) & 1 ? 2 : (cm & 1 ? 1 : 0));
    B.dst = (cm >> 5) & 7; B.src = (cm >> 8) & 7; B.logic = (cm >> 12) & 15;
    S.dither = (cm >> 2) & 1; S.colorUpdate = (cm >> 3) & 1; S.alphaUpdate = (cm >> 4) & 1;
    D.z.compare = zm & 1; D.z.func = (zm >> 1) & 7; D.z.update = (zm >> 4) & 1;
    D.alpha.ref0 = ac & 255; D.alpha.ref1 = (ac >> 8) & 255; D.alpha.comp0 = (ac >> 16) & 7; D.alpha.comp1 = (ac >> 19) & 7; D.alpha.op = (ac >> 22) & 3;
    S.dstAlpha = c1 & 255; S.dstAlphaEnable = (c1 >> 8) & 1;
    S.pixelFmt = pe & 7; S.zFmt16 = (pe >> 3) & 7; S.zCompBeforeTex = (pe >> 6) & 1;
  }
  if (dirty & kStateDirtyFog) decodeFog(r, S);

  // ---- viewport, scissor
  if (dirty & kStateDirtyView) {
    const uint32_t bo = bp(r, 0x59);
    const int offX = r.bpWritten[0x59] ? int(bo & 0x3FF) * 2 : 342, offY = r.bpWritten[0x59] ? int((bo >> 10) & 0x3FF) * 2 : 342;
    const float sx = xff(r, 0x101A), sy = xff(r, 0x101B), sz = xff(r, 0x101C), ox = xff(r, 0x101D), oy = xff(r, 0x101E), oz = xff(r, 0x101F);
    GxViewport& V = S.viewport;
    V.width = 2 * sx; V.height = -2 * sy;
    // Aurora GXTransform uses340 for viewport origins, GXCull uses342
    // for scissors. Sharing the scissor bias shifted the image two GX pixels.
    V.x = ox - sx - float(r.auroraViewport ? 340 : offX);
    V.y = oy + sy - float(r.auroraViewport ? 340 : offY);
    V.farZ = oz / 16777215.0f; V.nearZ = (oz - sz) / 16777215.0f;
    const uint32_t tl = bp(r, 0x20), br = bp(r, 0x21);
    GxScissor& C = S.scissor;
    C.x = int((tl >> 12) & 0x7FF) - offX; C.y = int(tl & 0x7FF) - offY;
    C.width = int((br >> 12) & 0x7FF) - offX - C.x + 1; C.height = int(br & 0x7FF) - offY - C.y + 1;
  }

  smark(12);
  // ---- textures
  if (dirty & kStateDirtyTex) {
  if (!all) D.texmapScaled = 0;
  for (int m = 0; m < 8; m++) {
    const int lo = m < 4 ? m : m - 4, base = m < 4 ? 0 : 0x20;
    const int idMode0 = 0x80 + base + lo, idMode1 = 0x84 + base + lo, idImg0 = 0x88 + base + lo, idImg3 = 0x94 + base + lo, idTlut = 0x98 + base + lo;
    GxTexBinding& T = S.tex[m];
    T.valid = r.bpWritten[idImg0];
    const uint32_t m0 = bp(r, idMode0), m1 = bp(r, idMode1), i0 = bp(r, idImg0), i3 = bp(r, idImg3), tl = bp(r, idTlut);
    const uint32_t key[7] = {m0, m1, i0, i3, tl, uint32_t(T.valid), r.texAddr[m]};
    TexMemo& tm = t_tex[m];
    if (g_stateMemo && tm.valid && sameKey(tm.key, key)) { T = tm.t; if (tm.scaled) D.texmapScaled |= uint8_t(1u << m); continue; }
    T.wrapS = m0 & 3; T.wrapT = (m0 >> 2) & 3;
    T.magLinear = (m0 >> 4) & 1;
    { const int mf = (m0 >> 5) & 7; T.minLinear = (mf & 4) != 0; T.mipMode = uint8_t(mf & 3); }
    T.edgeLod = !((m0 >> 8) & 1);
    T.lodBias = float(sext(m0 >> 9, 8)) / 32.0f;
    T.aniso = uint8_t((m0 >> 19) & 3);
    T.biasClamp = (m0 >> 21) & 1;
    T.minLod = float(m1 & 255) / 16.0f; T.maxLod = float((m1 >> 8) & 255) / 16.0f;
    T.width = int(i0 & 0x3FF) + 1; T.height = int((i0 >> 10) & 0x3FF) + 1; T.fmt = int((i0 >> 20) & 15);
    T.addr = r.texAddr[m] ? r.texAddr[m] : (i3 & 0x1FFFFF) << 5;   // Aurora: the full host address
    T.tlutOffset = tl & 0x3FF; T.tlutFmt = int((tl >> 10) & 3);
    const bool scaled = T.valid && (axisScale(planAxis(T.width, T.wrapS)) != 1.0f || axisScale(planAxis(T.height, T.wrapT)) != 1.0f);
    if (scaled) D.texmapScaled |= uint8_t(1u << m);
    if (g_stateMemo) { std::memcpy(tm.key, key, sizeof key); tm.t = T; tm.scaled = scaled; tm.valid = true; }
  }
  }
  smark(13);
  if (D.indirect) { if (tl_notes) S.notes.push_back("indirect texturing is configured"); }
}

GxCopy decodeCopy(const GxRegs& r) {
  GxCopy C;
  const uint32_t cmd = bp(r, 0x52), src = bp(r, 0x49), size = bp(r, 0x4A), dst = bp(r, 0x4B), stride = bp(r, 0x4D);
  C.toXfb = (cmd >> 14) & 1;
  C.clear = (cmd >> 11) & 1;
  C.halfScale = (cmd >> 9) & 1;
  C.intensity = (cmd >> 15) & 1;
  C.fmt = int((((cmd >> 3) & 1) << 3) | ((cmd >> 4) & 7));
  C.depth = (bp(r, 0x43) & 7) == 3;
  // XFB copies use their own source / size registers (0x49 / 0x4A are written for both in the SDK: the destination address tells them apart)
  C.srcX = int(src & 0x3FF); C.srcY = int((src >> 10) & 0x3FF);
  C.srcW = int(size & 0x3FF) + 1; C.srcH = int((size >> 10) & 0x3FF) + 1;
  C.dstAddr = r.copyDstAddr ? r.copyDstAddr : (dst & 0x1FFFFF) << 5;
  C.dstStrideBytes = (stride & 0x3FF) << 5;
  const uint32_t c0 = bp(r, 0x4F), c1 = bp(r, 0x50), cz = bp(r, 0x51);
  C.clearColor[0] = uint8_t(c0); C.clearColor[3] = uint8_t(c0 >> 8); C.clearColor[2] = uint8_t(c1); C.clearColor[1] = uint8_t(c1 >> 8);
  C.clearZ = cz & 0xFFFFFF;
  return C;
}

GxTlutLoad decodeTlutLoad(const GxRegs& r) {
  GxTlutLoad L;
  L.srcAddr = r.tlutSrcAddr ? r.tlutSrcAddr : (bp(r, 0x64) & 0x1FFFFF) << 5;
  const uint32_t v = bp(r, 0x65);
  L.tmemOffset = v & 0x3FF;
  L.entries = ((v >> 10) & 0x7FF) * 16;
  return L;
}

}  // namespace t2p
