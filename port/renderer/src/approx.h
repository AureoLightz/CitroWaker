// The last rung of the translator ladder (exact -> two passes -> approximate): a material that cannot be drawn exactly, not even in two passes, is degraded step by step
// until it can, and the draw is reported APPROX with the list of what was degraded and how far the result is from the original.
//
// Degradation steps, applied to the GX material (draw.h GxDrawState::tev):
//   zero      one input (a, b, c or d) of one channel of one stage becomes ZERO (colour channel slot c may also become ONE)
//   pass      a channel that writes Previous becomes a pass-through of Previous
//   erase     the whole stage is removed (its register writes with it: the register keeps its initial value)
// Each round ranks the steps that touch the stages the failed attempt names (all stages when the failure is global: too many stages, constants, a register crossing a cut) by
// the difference between the degraded and the original material on random inputs (the same model evaluator the translator is verified against), and takes the first one
// that makes the draw compile; when none does within the budget, the cheapest one is applied and the next round continues from there (every step removes something, so the
// loop ends). A final pass tries to take each step back again. The last resort is texture x vertex colour (one stage).
#pragma once
#include <string>

#include "draw.h"

namespace t2p {

struct MatErr { double mean255 = 0, max255 = 0; };   // per colour lane, in 1/255 units, 8 bit snapped outputs

// Difference between two TEV materials over `n` random inputs (deterministic for a seed).
MatErr materialError(const TevMaterial& original, const TevMaterial& degraded, uint64_t seed, int n);

// `failed`: the exact attempt's result (its notes name the stages that did not translate). On success `out` is an APPROX draw built from the degraded material, with the steps in its
// notes; on failure `why` says what stopped it (import blockers cannot be degraded away).
bool compileApprox(const GxDrawState&, const Options&, const PicaDraw& failed, PicaDraw& out, std::string& why);

}  // namespace t2p
