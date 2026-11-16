#include "texfit.h"

#include <algorithm>

namespace t2p {
namespace {
int nextPow2(int n) { int d = 1; while (d < n) d <<= 1; return d; }
bool isPow2(int n) { return n > 0 && (n & (n - 1)) == 0; }
std::string num(int v) { return std::to_string(v); }
}  // namespace

AxisPlan planAxis(int n, int gxWrap) {
  AxisPlan p;
  p.n = n;
  p.wrap = uint8_t(gxWrap);
  if (isPow2(n) && n >= 8) { p.D = n; p.fill = AxisPlan::Same; return p; }
  p.D = std::max(8, nextPow2(n));
  if (gxWrap == 0) {                                   // clamp
    p.fill = AxisPlan::EdgePad;
    return p;
  }
  if (isPow2(n)) {                                     // 1, 2, 4: tile exactly
    if (gxWrap == 1) { p.fill = AxisPlan::TileRepeat; p.wrap = 1; }
    else { p.fill = AxisPlan::TileMirror; p.wrap = 1; }   // [img, flip] repeated is a mirror
    return p;
  }
  p.fill = AxisPlan::Stretch;
  p.exact = false;
  p.note = std::string(gxWrap == 1 ? "repeat" : "mirror") + " wrap of a size-" + num(n) + " axis has no exact power-of-two form: stretched to " + num(p.D);
  return p;
}

int axisSource(const AxisPlan& p, int x, int nl, int Dl) {
  switch (p.fill) {
    case AxisPlan::Same: return x;
    case AxisPlan::EdgePad: return std::min(x, nl - 1);
    case AxisPlan::TileRepeat: return x % nl;
    case AxisPlan::TileMirror: { const int m = x % (2 * nl); return m < nl ? m : 2 * nl - 1 - m; }
    case AxisPlan::Stretch: return std::min(nl - 1, int((2LL * x + 1) * nl / (2LL * Dl)));
  }
  return x;
}

float axisScale(const AxisPlan& p) {
  return (p.fill == AxisPlan::Same || p.fill == AxisPlan::Stretch) ? 1.0f : float(double(p.n) / p.D);
}

}  // namespace t2p
