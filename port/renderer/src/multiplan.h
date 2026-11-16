// Plans for a two-pass draw (multipass.h): the scratch depth clear, the first pass into the scratch target and the second pass that reads it back.
// Plain data like c3dplan.h; backend3ds.cpp replays it (Backend3ds::drawTwoPass): scratch target -> clear -> pass 1 -> main target -> pass 2.
//
// Assumptions, unverified on hardware (hwcheck6 `mp` scene measures them, together with the `copy` scene's): the scratch target is a render target with the main target's size and
// format whose colour buffer is sampled as a W x H RGBA8 texture with u = (x_gpu + 1) / 2, v = (y_gpu + 1) / 2 of the GPU's own clip coordinates (the layout the EFB copy reads the main
// target with, efbcopy.h); a texel centre is hit exactly by the fragment centre, so nearest sampling returns the fragment's own pixel; the PICA's texture cache sees what the previous
// pass wrote after the target switch (C3D_FrameDrawOn flushes the buffer being left).
#pragma once
#include <string>
#include <vector>

#include "c3dplan.h"

namespace t2p {

struct C3dMultiPlan {
  bool ok = false;
  std::string why;
  bool hasClear = false;      // the first pass depth-tests on the scratch's own depth buffer: `clear` resets it first
  C3dPlan clear;              // depth-only quad over the draw's scissor
  C3dPlan first;              // into the scratch target
  C3dPlan second;             // into the main target; unit 0 = the scratch view
  C3dMultiPlan() = default;
  C3dMultiPlan(const C3dMultiPlan&) = delete;        // the plans point into the vectors below and into each other's storage: moving keeps them valid, copying would not
  C3dMultiPlan& operator=(const C3dMultiPlan&) = delete;
  C3dMultiPlan(C3dMultiPlan&&) = default;
  C3dMultiPlan& operator=(C3dMultiPlan&&) = default;
  std::vector<uint8_t> clearVerts;
  std::vector<uint16_t> clearIdx;
};

// `scratchView`: the entry through which the scratch target's colour buffer is sampled (Backend3ds::setMainView makes one for any render target).
C3dMultiPlan buildMultiPlan(const DrawPacket& P, C3dShaderCache& shaders, TextureCache& tex, const GuestMem& mem, TexEntry* scratchView,bool diagnostics = true);

}  // namespace t2p
