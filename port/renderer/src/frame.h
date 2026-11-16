// GX viewport / scissor / cull mode / viewport depth range -> the 3DS target.
//
// Geometry. GX NDC (x right, y up) maps to EFB pixels through the viewport: ex = vx + vw/2 (1 + nx), ey = vy + vh/2 (1 - ny). The port shows the EFB in an output
// rectangle of the logical (landscape, y down) target: lx = outX + ex * outW / efbW, ly = outY + ey * outH / efbH. The PICA framebuffer is rotated by 90 degrees,
// in the direction of citro3d's Mtx_OrthoTilt / Mtx_PerspTilt (read from its source): in NDC x_gpu = y_logical (up), y_gpu = -x_logical. With a 400x240 target the
// GPU window coordinates are then (wx, wy) = (TH - ly, TW - lx): the logical picture turned by 90 degrees.
// All of this is one affine clip-space matrix, `post`, applied after the GX projection:
//   X_g = a_y Y + b_y W,  Y_g = -(a_x X + b_x W),  Z and W untouched;  the vertex shader receives post * GX projection as projMtx.
// Orientation: the 2x2 block has determinant a_x a_y, so triangle winding is preserved unless the game uses a negative viewport height/width.
//
// Culling. GX front faces are CLOCKWISE (y up), per the GX SDK and a measured Wind Waker frame. hwcheck4 on Azahar: GPU_CULL_FRONT_CCW culls counter-clockwise triangles, GPU_CULL_BACK_CCW clockwise ones,
// with or without the tilt. So GX_CULL_FRONT -> BACK_CCW, GX_CULL_BACK -> FRONT_CCW, swapped when the matrix mirrors the image; GX_CULL_ALL draws nothing.
//
// Depth. GX: depth = far + (far - near) z/w for z/w in [-1, 0], i.e. the viewport depth range [near, far]. PICA z-buffer map: scale = far - near, offset = far.
//
// Scissor. GX scissor (EFB pixels) -> logical pixels whose centres fall inside (exact for the pixel-centre rule), clipped to the output rectangle, then transposed to
// the rectangle C3D_SetScissor(GPU_SCISSOR_NORMAL, left, top, right, bottom) takes. Verified on Azahar (hwcheck5 hypothesis A, hwcheck6 `scissor` scene exact):
// the arguments are in the GPU's y-up window, so the logical rectangle is flipped in both axes (see mapFrame). The colour buffer in memory is the standard 3DS
// layout (hwcheck6: buffer column X = TH - 1 - ly, row Y = lx, row 0 first in memory), which is what the display transfer sends to the screen unchanged.
#pragma once
#include <string>
#include <vector>

#include "gxstate.h"
#include "pipeline.h"

namespace t2p {

enum class FitMode { Pillarbox43, Stretch, Explicit };   // (Claude v80) Explicit: outX/outY/outW/outH are given (the bottom-screen UI route) and may exceed the target

struct FrameConfig {
  int efbW = 640, efbH = 448;          // size of the picture the game renders (viewport / scissor coordinates are in these pixels)
  int targetW = 400, targetH = 240;    // logical landscape target (3DS top screen)
  FitMode fit = FitMode::Pillarbox43;
  bool rotate = true;                  // screen target (rotated framebuffer). false: unrotated target (render-to-texture), window = logical
  // Widescreen (with fit = Stretch): the 4:3 picture fills the whole target, and every draw that renders to the full EFB width is squeezed back horizontally
  // by (4:3) / (target aspect) around the screen centre (buildDraw picks the factor per draw, mapFrame's xScale): 3D draws then keep their proportions and
  // show more at the sides (the GPU clips at the wider target edge, not at the game's 4:3 one); 2D draws (HUD) keep their proportions, centred; 2D draws that
  // span the whole width (fades, full-screen passes) are not squeezed and cover the target. Draws into part of the EFB (render-to-texture) are left alone.
  // The game culls objects outside its own 4:3 view, so they can appear late at the left and right edges.
  bool widescreen = false;
  // output rectangle inside the logical target, filled in by resolve()
  float outX = 0, outY = 0, outW = 400, outH = 240;
  void resolve();
};

struct PixelRect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool empty() const { return x1 <= x0 || y1 <= y0; } };   // x1 / y1 exclusive

struct PicaFrame {
  bool ok = false;                     // false: the viewport is degenerate or the cull mode is ALL: nothing to draw
  float post[4][4] = {};               // clip-space post matrix
  float proj[4][4] = {};               // post * GX projection: the projMtx uniform
  bool mirrored = false;               // the image is mirrored (negative viewport extent): winding is reversed
  int picaCull = 0;                    // 0 none, 1 GPU_CULL_FRONT_CCW, 2 GPU_CULL_BACK_CCW
  PixelRect scissorLogical;            // logical target pixels (y down), inside the output rectangle
  PixelRect scissorWindow;             // GPU window pixels (rotated like the picture when rotate; y flipped otherwise)
  PicaDepthMap depth;                  // the draw's depth map adjusted to the viewport depth range
  std::vector<std::string> notes;      // deviations (W-buffer with a depth range, degenerate viewport, ...)
};

// xScale: horizontal squeeze of the draw's picture around the viewport centre (FrameConfig::widescreen); 1 = none.
PicaFrame mapFrame(const GxState& S, const FrameConfig& cfg, const PicaDepthMap& drawDepth, float xScale = 1.0f);
// The xScale a draw gets under FrameConfig::widescreen (1 when off or when the draw does not render to the full EFB width).
float widescreenXScale(const GxState& S, const GxDraw& d, const FrameConfig& cfg);

// helpers (exposed for tests)
void mulMtx4(const float a[4][4], const float b[4][4], float out[4][4]);
PicaDepthMap depthMapZRange(float nearZ, float farZ);

}  // namespace t2p
