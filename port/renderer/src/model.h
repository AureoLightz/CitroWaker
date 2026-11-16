#include "material_vector.h"
// tev2pica: GameCube TEV -> PICA200 texenv translator + software verifier.
// Game-agnostic. Semantics of the PICA side follow Citra's shader generator
// (not yet checked against real hardware; see NOTES in README).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace t2p {

struct V3 { float v[3] = {0, 0, 0}; };      // alpha path uses lane 0 only
struct Rgba { float v[4] = {0, 0, 0, 0}; };

// ---------------------------------------------------------------- GameCube TEV
// One leaf id space for both channels. Legal color inputs: all but L_MID / L_PBUF / L_PBUFA.
// Legal alpha inputs: L_APREV, L_A0..A2, L_TEXA, L_RASA, L_KONST, L_ZERO.
enum Leaf : uint8_t {
  L_CPREV, L_APREV, L_C0, L_A0, L_C1, L_A1, L_C2, L_A2,
  L_TEXC, L_TEXA, L_RASC, L_RASA, L_ONE, L_HALF, L_ZERO, L_KONST,
  L_MID,  // internal: output of the previous PICA stage of a split
  L_PBUF, // internal: the PREV value from before the GC stage, read through the combiner buffer (translate() saves it there); rgb in the colour path, alpha in the alpha path
  L_PBUFA,// internal: the alpha of that old PREV (a colour path input APREV), read from the buffer's alpha part
  L_KONSTC,// the colour channel's constant selection (KCSEL), also in the alpha table: appears only as an operand of an alpha-channel R8 compare, which reads the colour inputs a and b
  L_MIDA, // internal, colour path only: the alpha lane of Previous broadcast (a compare's mask that was computed in the alpha lanes of earlier PICA stages)
  L_COUNT
};

struct KSel {  // GX_TEV_KCSEL / KASEL
  enum Kind : uint8_t { Fixed, Rgb, Chan } kind = Fixed;
  float fixed = 0;           // Fixed: 1, 7/8 ... 1/8
  uint8_t idx = 0, chan = 0; // Rgb/Chan: KCOLOR idx; Chan: 0..3 = r,g,b,a
};

struct TevChan {
  Leaf in[4] = {L_ZERO, L_ZERO, L_ZERO, L_ZERO};  // a b c d
  bool sub = false;              // out = d - lerp instead of d + lerp
  float bias = 0, scale = 1;     // bias 0/+-0.5, scale 0.5/1/2/4
  bool clamp = true;
  uint8_t out = 0;               // 0 = PREV, 1..3 = REG0..2
  // TEV compare mode (bias field 3): out = d + (a > b ? c : 0) with no bias / scale / sub, the clamp flag still applies. The comparison is on the 8 bit values (see tevCompareGT).
  //   0 = not a compare;  1 = R8_GT: compares the RED lanes of the colour inputs a and b (in the alpha channel in[0], in[1] hold the COLOUR channel's a and b, resolved
  //   with L_KONSTC for the colour constant selection: the hardware ignores the alpha inputs a and b in this mode);  2 = A8_GT (alpha channel only): compares the alpha inputs a and b.
  // The other kinds (EQ, GR16, BGR24, RGB8) are not modelled; gxstate raises an import blocker for them.
  uint8_t cmp = 0;
};

// texSwap/rasSwap: GX_SetTevSwapMode tables, output channel i takes input channel swap[i] (0=r 1=g 2=b 3=a)
struct TevStage {
  TevChan c, a; KSel kc, ka; int texmap = -1; int rasChan = 0;
  int texcoord = -1;   // GX texcoord feeding this stage's texture (used by draw.cpp to pick the PICA texture unit; -1 = none)
  uint8_t texSwap[4] = {0, 1, 2, 3}, rasSwap[4] = {0, 1, 2, 3};
  // translator annotation (not part of the GC data): this REG's rgb / alpha currently lives in the PICA combiner buffer
  bool regBufC[3] = {false, false, false}, regBufA[3] = {false, false, false};
};
struct TevMaterial { MaterialVector<TevStage> stages; };

// Runtime inputs. Everything in [0,1]; PREV starts at 0.
struct Inputs { Rgba tex[8], ras[2], kcolor[4], reg[3]; Rgba prev; };  // prev = initial PREV (0 for a whole material)

using LeafTable = std::array<V3, L_COUNT>;
// regs[0] = PREV, regs[1..3] = REG0..2
LeafTable buildLeaves(const Rgba regs[4], const Inputs&, const TevStage&, bool alphaPath);
V3 evalTevChan(const TevChan&, const LeafTable&);
float snap8(float);                                   // nearest level of the 8 bit grid: floor(x * 255 + 0.5) / 255 (exact half levels round up on both sides of every comparison)
bool tevCompareGT(const TevChan&, const LeafTable&);  // the decision of a compare channel (cmp != 0): a > b on the 8 bit values of the compared inputs
// An alpha channel in R8 compare mode reads the colour inputs a and b: copy them into the alpha channel's in[0], in[1] (constant selection KONST -> KONSTC). Call after both channels are set.
inline void setAlphaR8Operands(TevStage& st) {
  for (int j = 0; j < 2; j++) st.a.in[j] = st.c.in[j] == L_KONST ? L_KONSTC : st.c.in[j];
}
Rgba evalTev(const TevMaterial&, const Inputs&);

// ---------------------------------------------------------------- PICA200
enum PSrc : uint8_t { P_PRIMARY, P_TEX0, P_TEX1, P_TEX2, P_CONST, P_PREV, P_BUF };  // P_BUF = combiner buffer (hw source 0xD)
enum POp : uint8_t {
  OP_REPLACE, OP_MODULATE, OP_ADD, OP_ADDSIGNED, OP_LERP, OP_SUB, OP_MAD, OP_ADDMUL, OP_COUNT
};
// Alpha path may only use A,1A,R,1R,G,1G,B,1B. (^1 flips "one minus".)
enum POpr : uint8_t { OPR_RGB, OPR_1RGB, OPR_A, OPR_1A, OPR_R, OPR_1R, OPR_G, OPR_1G, OPR_B, OPR_1B };

// snap: model-only (not in the hardware words). The ideal-float evaluation rounds this operand to the 8 bit grid before use. The hardware holds every operand that way anyway; the
// flag is set on the operands of a compare's subtraction, where a decision (not a value) depends on the exact level and a constant such as HALF = 0.5 would otherwise sit
// between two levels in the ideal evaluation (the hardware register holds 128/255).
struct PArg { PSrc src = P_PREV; POpr opr = OPR_RGB; bool snap = false; };
struct PChan { POp op = OP_REPLACE; PArg arg[3]; uint8_t scale = 1; };  // scale 1/2/4

enum { SYM_NONE = 0, SYM_K0 = 1, SYM_R0 = 5 };  // K0..K3 = GX KCOLOR0..3, R0..R2 = initial REG0..2
struct KComp {  // one component of the per-stage constant color
  bool set = false;
  uint8_t sym = SYM_NONE, chan = 0;  // runtime-filled: symbol's channel
  bool inv = false;                  // store 1 - value
  float fixed = 0;                   // when sym == SYM_NONE
};
// updBufC/updBufA: this stage's output is copied into the combiner buffer's rgb / alpha (hardware: stages 0..3 only).
// The buffer update becomes visible two stages later: a stage j result is read through P_BUF at stage j+2 or later.
struct PStage { PChan c, a; KComp k[4]; bool updBufC = false, updBufA = false; };
struct PicaProgram { std::vector<PStage> stages; };

V3 picaChanEval(POp, const V3 args[3], uint8_t scale, bool quantize);
// quantize=false: ideal-float PICA (separates logic bugs from 8-bit rounding noise)
Rgba resolveConst(const PStage&, const Inputs&, bool quantize = true);
// globalStart=true: the program is a whole material, so PREV is 0 before stage 0 (in.prev ignored) and the
// hardware quirk applies: at stage 0 a "Previous" source in argument slot 0 or 1 reads slot 2's source instead
// (Azahar sw_rasterizer.cpp / glsl_fs_shader_gen.cpp force_source3). false: stage-local proof, PREV = in.prev.
// localBuf != nullptr (stage-local proofs): P_BUF reads that constant and the update flags/timing are ignored.
// Otherwise the combiner buffer follows Azahar: stage i reads the buffer as updated by stages <= i-2.
Rgba evalPica(const PicaProgram&, const Inputs&, bool quantize = true, bool globalStart = true,
              const Rgba* localBuf = nullptr);
uint32_t encodeBufferUpdate(const PicaProgram&);  // GPUREG_TEXENV_UPDATE_BUFFER (0x0E0) value: rgb mask << 8 | alpha mask << 12

struct HwStage { uint32_t sources, modifiers, ops, constColor, scales; };  // Citra TevStageConfig layout
HwStage encodeStage(const PStage&, const Rgba& resolvedConst);
extern const uint16_t kPicaTexEnvReg[6];  // GPUREG base of TexEnv 0..5

}  // namespace t2p
