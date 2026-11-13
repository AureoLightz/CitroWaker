// Fitting of one texture axis to the PICA's power-of-two sizes (texmap.h explains the policy). No dependencies, so the state decoder can use it too.
#pragma once
#include <cstdint>
#include <string>

namespace t2p {

struct AxisPlan {
  enum Fill { Same, EdgePad, TileRepeat, TileMirror, Stretch };
  int n = 0, D = 0;
  Fill fill = Same;
  uint8_t wrap = 0;                   // PICA wrap: 0 clamp, 1 repeat, 2 mirror
  bool exact = true;
  std::string note;
};
AxisPlan planAxis(int n, int gxWrap);   // gxWrap: 0 clamp, 1 repeat, 2 mirror
// Source index (0..nl-1) that the PICA texel x (0..Dl-1) of mip level l takes for this axis plan.
int axisSource(const AxisPlan& p, int x, int nl, int Dl);
// Image coordinate -> PICA coordinate scale: n/D when the image fills only part of the texture (padded / tiled), else 1.
float axisScale(const AxisPlan& p);

}  // namespace t2p
