// Internal: single-channel search. Finds PICA steps that reproduce one GC TEV channel.
#pragma once
#include <string>
#include <vector>

#include "alloc.h"
#include "translate.h"

namespace t2p {

// cmp: 0 = an ordinary operand; 1 = an operand of a compare's subtraction, read as the RED lane of the colour-context leaf (broadcast); 2 = the same for an alpha-valued leaf.
// Both are evaluated on the 8 bit grid (PArg::snap).
struct ArgSel { Leaf leaf = L_ZERO; bool inv = false; uint8_t cmp = 0; };
struct Step { POp op = OP_REPLACE; ArgSel arg[3]; uint8_t scale = 1; };
struct ChanSeq { std::vector<Step> steps; double err = 0; };  // err = max |error| (fraction) for approx

struct SrcInfo { PSrc src = P_PREV; POpr opr = OPR_RGB; bool isConst = false; Demand dem; };
// red: the leaf is read as its red lane only (the compared operands of an R8 compare). Colour-context leaves (TEXC, RASC, CPREV, C0..C2, KONSTC) are then also legal in the alpha path.
bool leafSource(Leaf, bool inv, bool alpha, const TevStage&, SrcInfo&, std::string& why, bool red = false);
// Which parts of the combiner buffer a sequence reads as the old PREV (L_PBUF / L_PBUFA): translate() must have the stage before the GC stage copy its output there.
enum { PREVBUF_RGB = 1, PREVBUF_ALPHA = 2 };
unsigned prevBufNeeds(const ChanSeq&, bool alphaChannel);
extern const int kArity[OP_COUNT];

struct ChanResult {
  Fidelity f = Fidelity::Unsupported;
  std::vector<ChanSeq> seqs;  // alternatives, best first; 1 or 2 steps, a compare takes 4 (hoistMask: the select steps only)
  std::vector<Step> mask;     // hoistMask: the three alpha-path steps that compute the compare's 0/1 mask into Previous's alpha lane
  std::string note;
};
// hoistMask (colour R8 compare only): the mask is computed by `mask` in the ALPHA lanes of three earlier PICA stages (the caller places them, see translateHoisted), so
// the sequences are just the select d + mask * c, reading the mask through L_MIDA and the old PREV directly.
// minSteps >= 2 (not for compares): skip the single-stage solutions and return the exact two- or three-stage forms, for a caller whose single-stage solutions did not fit the
// stage's shared constant (the other channel needs the same components).
ChanResult solveChan(const TevStage&, bool alpha, double approxLimit, uint64_t seed, bool hoistMask = false, int minSteps = 1);

}  // namespace t2p
