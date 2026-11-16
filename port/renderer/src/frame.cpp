#include "frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace t2p {

void FrameConfig::resolve() {
  if (fit == FitMode::Explicit) return;
  if (fit == FitMode::Stretch) { outX = 0; outY = 0; outW = float(targetW); outH = float(targetH); return; }
  outH = float(targetH);
  outW = std::min(float(targetW), float(targetH) * 4.0f / 3.0f);
  outX = (float(targetW) - outW) / 2.0f;
  outY = 0;
}

void mulMtx4(const float a[4][4], const float b[4][4], float out[4][4]) {
  float t[4][4];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      double s = 0;
      for (int k = 0; k < 4; k++) s += double(a[r][k]) * double(b[k][c]);
      t[r][c] = float(s);
    }
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r][c] = t[r][c];
}

PicaDepthMap depthMapZRange(float nearZ, float farZ) {
  PicaDepthMap m = depthMapZ();
  m.scale = farZ - nearZ;
  m.offset = farZ;
  return m;
}

namespace {
int ceilToInt(double v) { return int(std::ceil(v - 1e-9)); }
}

float widescreenXScale(const GxState& S, const GxDraw& d, const FrameConfig& cfgIn) {
  if (!cfgIn.widescreen) return 1.0f;
  FrameConfig cfg = cfgIn;
  cfg.resolve();
  const GxViewport& V = S.viewport;
  if (V.x > 0.5f || V.x + V.width < float(cfg.efbW) - 0.5f) return 1.0f;   // part of the EFB: render-to-texture
  const float s = float((4.0 / 3.0) / (double(cfg.outW) / double(cfg.outH)));
  if (s >= 1.0f) return 1.0f;
  if (!S.proj.ortho) return s;                                                 // 3D: wider view
  // 2D: squeeze (keep proportions, centred) unless the draw spans the whole width
  float lo = 1e30f, hi = -1e30f;
  const float (*P)[4] = S.uniformView().proj;
  for (uint32_t i = 0; i < d.v.count && d.v.hasPos; i++) {
    const int m = d.v.hasPmIdx ? std::min(9, d.v.pmIdx[i] / 3) : 0;
    const float (*M)[4] = S.uniformView().pos[m];
    const float* p = &d.v.pos[size_t(i) * 3];
    float e[4];
    for (int r = 0; r < 3; r++) e[r] = M[r][0] * p[0] + M[r][1] * p[1] + M[r][2] * p[2] + M[r][3];
    e[3] = 1;
    const float x = P[0][0] * e[0] + P[0][1] * e[1] + P[0][2] * e[2] + P[0][3] * e[3];
    const float w = P[3][0] * e[0] + P[3][1] * e[1] + P[3][2] * e[2] + P[3][3] * e[3];
    if (w == 0) continue;
    lo = std::min(lo, x / w); hi = std::max(hi, x / w);
  }
  return (lo <= -0.97f && hi >= 0.97f) ? 1.0f : s;
}

namespace {
PicaFrame mapFrameCompute(const GxState& S, const FrameConfig& cfgIn, const PicaDepthMap& drawDepth, float xScale);
// Everything mapFrameCompute reads, bit for bit (consecutive draws mostly share their viewport, projection and scissor).
struct MapFrameKey {
  float vp[6]; float proj[4][4]; int sc[4]; int cull; uint8_t ortho, zc, zf, ws, rot, zb; int fit;
  int efb[2], tgt[2]; float out[4]; float dScale, dOffset; uint32_t dClear; float xScale;
};
void makeKey(MapFrameKey& k, const GxState& S, const FrameConfig& c, const PicaDepthMap& d, float xScale) {
  std::memset(&k, 0, sizeof k);
  const GxViewport& V = S.viewport;
  k.vp[0] = V.x; k.vp[1] = V.y; k.vp[2] = V.width; k.vp[3] = V.height; k.vp[4] = V.nearZ; k.vp[5] = V.farZ;
  std::memcpy(k.proj, S.uniformView().proj, sizeof k.proj);
  k.sc[0] = S.scissor.x; k.sc[1] = S.scissor.y; k.sc[2] = S.scissor.width; k.sc[3] = S.scissor.height;
  k.cull = S.cullMode; k.ortho = S.proj.ortho; k.zc = S.draw.z.compare; k.zf = uint8_t(S.draw.z.func);
  k.ws = c.widescreen; k.rot = c.rotate; k.fit = int(c.fit); k.efb[0] = c.efbW; k.efb[1] = c.efbH; k.tgt[0] = c.targetW; k.tgt[1] = c.targetH;
  k.out[0] = c.outX; k.out[1] = c.outY; k.out[2] = c.outW; k.out[3] = c.outH;
  k.zb = d.zBuffer; k.dScale = d.scale; k.dOffset = d.offset; k.dClear = d.clear; k.xScale = xScale;
}
}  // namespace

PicaFrame mapFrame(const GxState& S, const FrameConfig& cfgIn, const PicaDepthMap& drawDepth, float xScale) {
  thread_local MapFrameKey lastKey; thread_local PicaFrame last; thread_local bool valid = false;
  MapFrameKey k; makeKey(k, S, cfgIn, drawDepth, xScale);
  if (valid && std::memcmp(&k, &lastKey, sizeof k) == 0) return last;
  last = mapFrameCompute(S, cfgIn, drawDepth, xScale); std::memcpy(&lastKey, &k, sizeof k); valid = true;
  return last;
}

namespace {
PicaFrame mapFrameCompute(const GxState& S, const FrameConfig& cfgIn, const PicaDepthMap& drawDepth, float xScale) {
  PicaFrame F;
  FrameConfig cfg = cfgIn;
  cfg.resolve();
  const GxViewport& V = S.viewport;

  // ---- culling
  switch (S.cullMode) {
    case 0: F.picaCull = 0; break;
    case 1: F.picaCull = 2; break;
    case 2: F.picaCull = 1; break;
    default: F.notes.push_back("GX_CULL_ALL: the draw is skipped"); return F;
  }
  if (!(V.width != 0 && V.height != 0)) { F.notes.push_back("viewport has zero width or height: the draw is skipped"); return F; }

  // ---- clip-space post matrix
  const double kx = double(cfg.outW) / cfg.efbW, ky = double(cfg.outH) / cfg.efbH;
  const double TW = cfg.targetW, TH = cfg.targetH;
  const double ax = kx * V.width / TW * double(xScale);
  const double bx = 2.0 * (cfg.outX + kx * (V.x + V.width / 2.0)) / TW - 1.0;
  const double ay = ky * V.height / TH;
  const double by = 1.0 - 2.0 * (cfg.outY + ky * (V.y + V.height / 2.0)) / TH;
  // logical NDC: Lx = ax nx + bx, Ly = ay ny + by
  float P[4][4] = {};
  if (cfg.rotate) {   // citro3d tilt: x_g = Ly, y_g = -Lx
    P[0][1] = float(ay);  P[0][3] = float(by);
    P[1][0] = float(-ax); P[1][3] = float(-bx);
  } else {            // unrotated: x_g = Lx, y_g = Ly
    P[0][0] = float(ax);  P[0][3] = float(bx);
    P[1][1] = float(ay);  P[1][3] = float(by);
  }
  P[2][2] = 1.0f; P[3][3] = 1.0f;
  std::copy(&P[0][0], &P[0][0] + 16, &F.post[0][0]);
  mulMtx4(F.post, S.uniformView().proj, F.proj);
  F.mirrored = ax * ay < 0;   // determinant of the xy block, rotated or not
  // GX front faces are CLOCKWISE in GX NDC (y up), as the GX SDK documents: measured on a Wind Waker frame (the visible sky and terrain under GX_CULL_BACK are
  // all clockwise there). GPU_CULL_FRONT_CCW (1) culls counter-clockwise triangles, GPU_CULL_BACK_CCW (2) clockwise ones (hwcheck4), so CULL_BACK -> 1, CULL_FRONT -> 2.
  if (S.cullMode == 1) F.picaCull = F.mirrored ? 1 : 2;
  else if (S.cullMode == 2) F.picaCull = F.mirrored ? 2 : 1;

  // ---- scissor: logical pixels whose centres lie inside the GX rectangle
  {
    const GxScissor& C = S.scissor;
    PixelRect L;
    // (Claude v108) the widescreen squeeze (xScale, around the viewport centre) moves the geometry, so a partial scissor moves with it: the swim
    // air meter's water fill (J2D clip pane on the right of the HUD) was cut to a sliver. A full-width scissor stays full width.
    double sx0 = C.x, sx1 = double(C.x) + C.width;
    if (xScale != 1.0f && (C.x > 0 || double(C.x) + C.width < double(cfg.efbW))) {
      const double vc = double(V.x) + double(V.width) / 2.0;
      sx0 = vc + double(xScale) * (sx0 - vc); sx1 = vc + double(xScale) * (sx1 - vc);
      if (sx0 > sx1) std::swap(sx0, sx1);
    }
    L.x0 = ceilToInt(cfg.outX + sx0 * kx - 0.5);
    L.x1 = ceilToInt(cfg.outX + sx1 * kx - 0.5);
    L.y0 = ceilToInt(cfg.outY + C.y * ky - 0.5);
    L.y1 = ceilToInt(cfg.outY + (C.y + C.height) * ky - 0.5);
    const int ox0 = ceilToInt(cfg.outX - 0.5), ox1 = ceilToInt(cfg.outX + cfg.outW - 0.5);
    const int oy0 = ceilToInt(cfg.outY - 0.5), oy1 = ceilToInt(cfg.outY + cfg.outH - 0.5);
    L.x0 = std::max(L.x0, ox0); L.x1 = std::min(L.x1, ox1);
    L.y0 = std::max(L.y0, oy0); L.y1 = std::min(L.y1, oy1);
    // (Claude v80) and to the target (a no-op unless FitMode::Explicit: the other modes' output rectangle lies inside the target)
    L.x0 = std::max(L.x0, 0); L.x1 = std::min(L.x1, cfg.targetW);
    L.y0 = std::max(L.y0, 0); L.y1 = std::min(L.y1, cfg.targetH);
    if (L.empty()) L = PixelRect{};
    F.scissorLogical = L;
    if (cfg.rotate) F.scissorWindow = PixelRect{cfg.targetH - L.y1, cfg.targetW - L.x1, cfg.targetH - L.y0, cfg.targetW - L.x0};
    else F.scissorWindow = PixelRect{L.x0, cfg.targetH - L.y1, L.x1, cfg.targetH - L.y0};
    if (L.empty()) F.scissorWindow = PixelRect{};
  }

  // ---- depth range
  F.depth = drawDepth;
  if (drawDepth.zBuffer) {
    F.depth.scale = drawDepth.scale * (V.farZ - V.nearZ);
    F.depth.offset = drawDepth.offset * V.farZ;   // drawDepth is the full-range map (scale 1, offset 1): depth = z/w + 1
  } else if (V.nearZ != 0.0f || V.farZ != 1.0f) {
    // W-buffer (fog frames): depth = z*scale + w*offset has no constant term, so only [0, far] ranges can be expressed
    F.depth.scale = drawDepth.scale * V.farZ;
    F.depth.offset = drawDepth.offset * V.farZ;
    if (V.nearZ != 0.0f) F.notes.push_back("W-buffer depth with a viewport depth range that does not start at 0 is approximated by [0, far]");
  }
  if (!drawDepth.zBuffer && S.proj.ortho && S.draw.z.compare && S.draw.z.func != 7)
    F.notes.push_back("W-buffer frame with an orthographic draw: its fragments all get the same depth, so its depth test cannot order its own geometry");
  if (g_picaDepthFold) {   // (Claude v90) the map goes into the projection (pipeline.h); F.post stays the xy mapping (efbcopy reuses it)
    if (drawDepth.zBuffer && picaReverseZ()) {   // (Claude v107) stored = kPicaDepthMax - (s z/w + o): z' = s z + (o - kPicaDepthMax) w (usually just s z)
      const double s = F.depth.scale, o = double(F.depth.offset) - double(kPicaDepthMax);
      for (int j = 0; j < 4; j++) F.proj[2][j] = float(s * F.proj[2][j] + o * F.proj[3][j]);
    } else
    picaFoldDepthRow(F.proj[2], F.proj[3], F.depth.scale, F.depth.offset);
    F.depth.scale = -1.0f; F.depth.offset = 0.0f;
  }
  F.ok = true;
  return F;
}
}  // namespace

}  // namespace t2p
