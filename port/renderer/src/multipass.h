// Two-pass draws. A GX material is a chain of up to 16 TEV stages over 8 textures; the PICA has 6 texenv stages and 3 texture units. Most long materials are rescued by the
// translator itself (compare masks hoisted into the alpha lanes, register roles, ...); the rest are cut once and drawn twice over the same geometry:
//
//   pass 1  GX stages [0, cut) -> a scratch target (RGBA8, the main target's size; its own depth buffer when the draw depth-tests).
//   pass 2  a stage that reads the scratch at the fragment's own pixel (texture unit 0, the SCREEN texgen of vtx.h: nearest, clamped, one texel per pixel) followed by GX stages
//           [cut, n); this pass carries the draw's blend, alpha test, fog, depth and colour masks.
//
// Why this is exact for the material: PREV after a stage is always a multiple of 1/255 in all four lanes (the combiner rounds), so an RGBA8 target holds it losslessly, and PREV is the
// only value that crosses the cut (a TEV register written before it and read after it is rebuilt in the second pass from copies of its writing stages, or the cut is refused when
// its value depends on Previous; crossingRegs). Both parts go through the ordinary translator, so each is
// proven against the TEV model on its own; test_multipass.cpp checks the composition against the whole-material model.
//
// What it cannot promise is the per-fragment behaviour of the draw's fixed-function state, because pass 2 sees one scratch value per pixel while GX evaluates fragment by fragment:
//   * opaque draws (no blend, no alpha test) whose depth test is ordered (LESS / LEQUAL / GREATER / GEQUAL with depth write) or absent / ALWAYS are exact: pass 1 keeps, per pixel, the
//     fragment the GX draw would end with (nearest wins on its own depth buffer, last wins without one), and that is the fragment pass 2 keeps;
//   * blending, an alpha test (a fragment that fails it in pass 2 does not hide the one behind it in pass 1), depth without write, and EQUAL / NOTEQUAL depth functions let fragments of
//     the same draw that overlap on screen see the first pass's value of another fragment. The draw is then Approx and says so.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "draw.h"

namespace t2p {

// (register, channel) pairs - register 0 = PREV, 1..3 = REG0..2; channel 0 = colour, 1 = alpha - that a stage from `cut` on reads before writing it although a stage before `cut` wrote it.
// PREV crosses through the scratch; a REG crosses by being rebuilt in the second pass (makeSplit), which needs a value that does not depend on Previous.
std::vector<std::pair<int, int>> crossingRegs(const TevMaterial& t, int cut);

struct SplitDraws {
  GxDrawState first, second;     // the two GX-level draws (second.vtx.texgen.back() is the SCREEN texgen, second.tev.stages[0] the scratch read)
  bool depthOrdered = false;     // pass 1 depth-tests on the scratch's own depth buffer
  bool depthFar = true;          // ... cleared to far (LESS / LEQUAL) or near (GREATER / GEQUAL)
  bool exactFragments = true;    // the draw state keeps one fragment per pixel the same way in both passes (see above)
  std::string fragmentNote;      // why not
  std::string rematNote;         // registers that cross the cut and are rebuilt in the second pass (they do not depend on Previous)
};

// Builds the two GX-level draws for a cut after stage `cut - 1` (1 <= cut < stage count). false: the cut is not possible (why).
bool makeSplit(const GxDrawState& g, int cut, SplitDraws& out, std::string& why);

// Tries the cuts in the order of balance and keeps the first one for which both passes compile Exact, else the best one that compiles at all. `g` must be a draw that compileDrawSingle
// refused. true: `out` is the first pass with pass2 set (draw.h).
bool compileSplit(const GxDrawState& g, const Options& opt, PicaDraw& out, std::string& why);

}  // namespace t2p
