// EFB copies (GXCopyTex / GXCopyDisp) and copy-clears on the 3DS.
//
// The GameCube renders into the EFB, then copies a rectangle of it into main memory as a texture (GXCopyTex) or as the next video frame (GXCopyDisp), optionally
// clearing the EFB afterwards (the clear colour / depth are registers of the copy). Games use texture copies for reflections, heat haze, shadows, bloom, ...
// and read them back with ordinary texture bindings at the copy's destination address.
//
// The port's EFB is the main render target (rotated 240x400 RGBA8 colour buffer + depth), shown at half the EFB's resolution (FrameConfig). A texture copy is
//   * a draw into a new VRAM texture (its own render target) that samples the main target's colour buffer as a texture and writes the picture the GX copy format
//     would produce: the format table (efbCopyFormat) chooses the target's PICA format and the combiner program that converts (luma for I4 / I8 / IA4 / IA8, a single
//     channel for R8 / G8 / B8 / A8 / R4, RGB565 and RGBA4 targets for the quantised formats);
//   * registered in the texture cache (TextureCache::addRendered) at the destination address, so the next binding of that address / size finds the GPU texture and
//     guest memory is never consulted. The picture has the resolution of the port's EFB (copy rectangle * outW / efbW), not the EFB's own: a copy of 320x240
//     EFB pixels is a 160x120 texture on the 3DS (a half-scale copy, the usual case for GXSetTexCopyDst(mipmap = TRUE), is exactly one texel per target pixel);
//   * the texture's allocation is a power of two (PICA); the picture sits in its top left corner and the padding replicates the picture's edge, so clamped
//     sampling and bilinear filtering at the edge behave like the GX texture's. RenderedTexture::portW/H and the entry's scaleS/scaleT tell the draw that samples it.
// The clear is a draw into the main target: a quad over the whole EFB restricted by a scissor to the copy rectangle, writing the clear colour / depth through the
// colour, alpha and depth write enables the copy executes with (the SDK sets "depth test always" and "no blending" around the copy; colour / alpha / depth update
// stay as the game set them, and so does the hardware's clear, which writes the same planes).
//
// Everything here is plain data (no GPU calls) like c3dplan.h: the host-tested planner decides what to draw, backend3ds.cpp replays it.
//
// Assumptions, unverified on hardware (hwcheck6 `copy` scene measures them): the framebuffer is sampled as a 240x400 NPOT texture with u = 1 - ly / targetH,
// v = 1 - lx / targetW (the layout hwcheck6 measured for the buffer, row 0 = v 1); a render-to-texture target stores NDC y = +1 in memory row 0 (the orientation
// citro2d's render-to-texture relies on); RGB565 / RGBA4 targets quantise like the GX copy (truncation); luma = 0.257 R + 0.504 G + 0.098 B + 16/255 (Dolphin's
// EFB copy shader; BT.601 studio swing); the clear covers the copy rectangle.
#pragma once
#include <string>
#include <vector>

#include "c3dplan.h"
#include "model.h"

namespace t2p {

// What the copy writes, as combiner programs on the sampled EFB colour (R G B A) -> the texel the GX format's sampler would return.
enum class CopyProg : uint8_t {
  Unsupported,
  Color,        // R G B A (A = 1 when the EFB has no alpha)                                  RGBA8, RGB565, RGB5A3
  ChanR,        // R R R R                                                                   R8, R4         (sampled as I8 / I4)
  ChanG,        // G G G G                                                                   G8
  ChanB,        // B B B B                                                                   B8
  ChanA,        // A A A A                                                                   A8
  ChanRA,       // R R R A                                                                   RA8, RA4       (sampled as IA8 / IA4)
  Luma,         // Y Y Y Y with Y = 0.257 R + 0.504 G + 0.098 B + 16/255                      I8, I4
  LumaA,        // Y Y Y A                                                                   IA8, IA4
};

struct EfbCopyFormat {
  bool known = false;
  const char* name = "";
  CopyProg prog = CopyProg::Unsupported;
  int pfmt = PTF_RGBA8;        // the render target's colour format (PICA numbering: RGBA8 0, RGB8 1, RGB5A1 2, RGB565 3, RGBA4 4)
  int sampleFmt = 6;           // GX texture format a binding reads the copy with (decides the size of the image in guest memory)
  std::string note;            // what the target format cannot reproduce
};

EfbCopyFormat efbCopyFormat(const GxCopy& c);

// The combiner program of a copy (stages 0..n-1; the rest of the six pass through). hasAlpha: the EFB stores alpha (RGBA6_Z24); otherwise alpha reads 1.
PicaProgram efbCopyProgram(CopyProg p, bool hasAlpha);
// Fragment primary colour the programs expect (16/255 in all four channels: the luma offset).
inline constexpr float kEfbCopyPrimary = 16.0f / 255.0f;

struct EfbCopyOptions {
  float frameFogRange = 0;     // > 0: the frame uses a W-buffer with this range (Options::frameFogRange): the clear writes its depth through that map
};

struct EfbCopyPlan {
  bool ok = false;             // false: nothing is done (why)
  std::string why;
  bool toXfb = false;          // GXCopyDisp: the port presents the main target itself; only the clear remains
  bool toTexture = false;

  // ---- texture copy
  RenderedTexture tex;         // register with TextureCache::addRendered; the host allocates tex.allocW x tex.allocH in format tex.pfmt (VRAM, renderable) and draws `blit` into it
  int srcLogicalX0 = 0, srcLogicalY0 = 0, srcLogicalX1 = 0, srcLogicalY1 = 0;   // the sampled rectangle of the main target (logical pixels, rounded out; for logs)
  C3dPlan blit;                // unit 0 = the main target's colour buffer (TexEntry passed to planEfbCopy); the target has no depth buffer; viewport / scissor = the whole texture

  // ---- clear
  bool clears = false;
  C3dPlan clear;               // into the main target after the blit
  bool clearsColor = false, clearsAlpha = false, clearsDepth = false;
  uint8_t clearColor[4] = {};  // the copy's clear registers (GXSetCopyClear), also what an XFB copy hands to the next frame's clear
  uint32_t clearZ = 0;

  std::vector<std::string> notes;   // approximations of this copy

  EfbCopyPlan() = default;
  EfbCopyPlan(const EfbCopyPlan&) = delete;                 // the plans point into the vectors below: moving keeps them valid, copying would not
  EfbCopyPlan& operator=(const EfbCopyPlan&) = delete;
  EfbCopyPlan(EfbCopyPlan&&) = default;
  EfbCopyPlan& operator=(EfbCopyPlan&&) = default;
  std::vector<uint8_t> blitVerts, clearVerts;
  std::vector<uint16_t> blitIdx, clearIdx;
};

// `at`: the GX state when the copy command executes (decodeState of the register file at onCopy: colour / alpha / depth update, EFB pixel format).
// `mainView`: the texture entry through which the main target's colour buffer is sampled (the host makes it: width = cfg.targetH, height = cfg.targetW, RGBA8).
EfbCopyPlan planEfbCopy(const GxCopy& c, const GxState& at, const FrameConfig& cfg, C3dShaderCache& shaders, TexEntry* mainView, const EfbCopyOptions& opt = {});

// The shader sources (exposed for tests).
std::string efbBlitShaderSource();
// (Claude v106) a textured mesh drawn into a w x h target without depth: 20-byte vertices (clip position xyz, texture coordinate st), unit 0 = `tex`
// sampled with repeat wrapping, linear filtering and linear mip filtering up to `maxLevel`; colour and alpha straight from the texture. The plan borrows
// `verts` / `idx`. The renderer's texture synthesis (renderer3ds.cpp warpOne) uses it. false: `why`.
bool planTexturedMesh(C3dPlan& L, C3dShaderCache& shaders, TexEntry* tex, int maxLevel, const std::vector<uint8_t>& verts, const std::vector<uint16_t>& idx, int w, int h, std::string& why);
std::string efbClearShaderSource();

// Bytes of the GX image a copy of gxW x gxH texels of the given texture format occupies in guest memory.
uint32_t efbCopyGuestBytes(int sampleFmt, int gxW, int gxH);

}  // namespace t2p
