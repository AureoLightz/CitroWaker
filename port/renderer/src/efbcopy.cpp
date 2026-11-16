#include "efbcopy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace t2p {
namespace {

int pow2ceil8(int v) { int p = 8; while (p < v) p <<= 1; return p; }

int bytesPerPixel(int pfmt) { return pfmt == PTF_RGBA8 ? 4 : pfmt == PTF_RGB8 ? 3 : 2; }

void putf(std::vector<uint8_t>& v, float x) { const size_t o = v.size(); v.resize(o + 4); std::memcpy(&v[o], &x, 4); }

// ---------------------------------------------------------------- combiner stages
constexpr PArg tex(POpr o) { return PArg{P_TEX0, o}; }
constexpr PArg konst(POpr o) { return PArg{P_CONST, o}; }
constexpr PArg prev(POpr o) { return PArg{P_PREV, o}; }
constexpr PArg primary(POpr o) { return PArg{P_PRIMARY, o}; }

void setC(PStage& s, POp op, PArg a0, PArg a1 = PArg{P_PREV, OPR_RGB}, PArg a2 = PArg{P_PREV, OPR_RGB}) { s.c.op = op; s.c.arg[0] = a0; s.c.arg[1] = a1; s.c.arg[2] = a2; }
void setA(PStage& s, POp op, PArg a0, PArg a1 = PArg{P_PREV, OPR_A}, PArg a2 = PArg{P_PREV, OPR_A}) { s.a.op = op; s.a.arg[0] = a0; s.a.arg[1] = a1; s.a.arg[2] = a2; }
void constRgb(PStage& s, float v) { for (int i = 0; i < 3; i++) { s.k[i].set = true; s.k[i].sym = SYM_NONE; s.k[i].fixed = v; } }
void constA(PStage& s, float v) { s.k[3].set = true; s.k[3].sym = SYM_NONE; s.k[3].fixed = v; }

// BT.601 studio swing Y = 0.257 R + 0.504 G + 0.098 B + 16/255 as three 8 bit constants. Each combiner stage rounds to 8 bits, so the constants are chosen against that model, not
// by rounding each weight: (65, 129, 25) / 255 against the GX formula floor(Y + 0.5) leaves 64% of all colours exact, mean error 0.36, worst 2 (rounding each weight to 66 / 129 / 25:
// 49% exact, mean 0.54, worst 2). The offset is the vertex colour (kEfbCopyPrimary).
const float kLumaR = 65.0f / 255.0f, kLumaG = 129.0f / 255.0f, kLumaB = 25.0f / 255.0f;

}  // namespace

// ---------------------------------------------------------------- format table
EfbCopyFormat efbCopyFormat(const GxCopy& c) {
  EfbCopyFormat f;
  auto make = [&](const char* name, CopyProg p, int pfmt, int sampleFmt, const char* note = "") {
    f.known = true; f.name = name; f.prog = p; f.pfmt = pfmt; f.sampleFmt = sampleFmt; f.note = note;
  };
  static const char* const q4 = "4-bit format: the RGBA4 target quantises the channels (its rounding is unverified; GX truncates)";
  if (c.depth) { f.name = "Z"; f.note = "depth copies are not implemented"; return f; }
  if (c.intensity) {
    switch (c.fmt) {
      case 0: make("I4", CopyProg::Luma, PTF_RGBA4, 0, q4); break;
      case 1: make("I8", CopyProg::Luma, PTF_RGBA8, 1); break;
      case 2: make("IA4", CopyProg::LumaA, PTF_RGBA4, 2, q4); break;
      case 3: make("IA8", CopyProg::LumaA, PTF_RGBA8, 3); break;
      default: f.name = "YUVA8"; f.note = "this intensity copy format is not implemented"; break;
    }
    return f;
  }
  switch (c.fmt) {
    case 0: make("R4", CopyProg::ChanR, PTF_RGBA4, 0, q4); break;
    case 2: make("RA4", CopyProg::ChanRA, PTF_RGBA4, 2, q4); break;
    case 3: make("RA8", CopyProg::ChanRA, PTF_RGBA8, 3); break;
    case 4: make("RGB565", CopyProg::Color, PTF_RGB565, 4, "RGB565 target: the 5/6-bit quantisation is the PICA output merger's (rounding unverified; GX truncates)"); break;
    case 5: make("RGB5A3", CopyProg::Color, PTF_RGBA8, 5, "RGB5A3 is kept at RGBA8: its 5/4-bit quantisation is not emulated"); break;
    case 6: make("RGBA8", CopyProg::Color, PTF_RGBA8, 6); break;
    case 1: case 8: make("R8", CopyProg::ChanR, PTF_RGBA8, 1); break;   // (nibble 1 without the intensity bit is an alias of R8; with a Z copy it is Z8)
    case 7: make("A8", CopyProg::ChanA, PTF_RGBA8, 1); break;
    case 9: make("G8", CopyProg::ChanG, PTF_RGBA8, 1); break;
    case 10: make("B8", CopyProg::ChanB, PTF_RGBA8, 1); break;
    case 11: f.name = "RG8"; f.note = "RG8 copies are not implemented"; break;
    case 12: f.name = "GB8"; f.note = "GB8 copies are not implemented"; break;
    default: f.name = "?"; f.note = "copy format nibble " + std::to_string(c.fmt) + " is not implemented"; break;
  }
  return f;
}

uint32_t efbCopyGuestBytes(int sampleFmt, int gxW, int gxH) { return uint32_t(gxTextureBytes(sampleFmt, gxW, gxH)); }

// ---------------------------------------------------------------- programs
PicaProgram efbCopyProgram(CopyProg p, bool hasAlpha) {
  PicaProgram prog;
  PStage s0;
  switch (p) {
    case CopyProg::Color:
      setC(s0, OP_REPLACE, tex(OPR_RGB));
      if (hasAlpha) setA(s0, OP_REPLACE, tex(OPR_A)); else { setA(s0, OP_REPLACE, konst(OPR_A)); constA(s0, 1.0f); }
      prog.stages.push_back(s0);
      break;
    case CopyProg::ChanR: case CopyProg::ChanG: case CopyProg::ChanB: case CopyProg::ChanA: {
      const POpr o = p == CopyProg::ChanR ? OPR_R : p == CopyProg::ChanG ? OPR_G : p == CopyProg::ChanB ? OPR_B : OPR_A;
      if (p == CopyProg::ChanA && !hasAlpha) {   // alpha reads 1: the texel is white
        setC(s0, OP_REPLACE, konst(OPR_RGB)); constRgb(s0, 1.0f);
        setA(s0, OP_REPLACE, konst(OPR_A)); constA(s0, 1.0f);
      } else {
        setC(s0, OP_REPLACE, tex(o));
        setA(s0, OP_REPLACE, tex(o));
      }
      prog.stages.push_back(s0);
      break;
    }
    case CopyProg::ChanRA:
      setC(s0, OP_REPLACE, tex(OPR_R));
      if (hasAlpha) setA(s0, OP_REPLACE, tex(OPR_A)); else { setA(s0, OP_REPLACE, konst(OPR_A)); constA(s0, 1.0f); }
      prog.stages.push_back(s0);
      break;
    case CopyProg::Luma: case CopyProg::LumaA: {
      // Y = wR R + wG G + wB B + 16/255 as three multiply-adds: colour operand SRC_R / SRC_G / SRC_B replicates the channel; the offset rides in on the vertex colour
      const bool a = p == CopyProg::LumaA;
      const float w[3] = {kLumaR, kLumaG, kLumaB};
      const POpr ch[3] = {OPR_R, OPR_G, OPR_B};
      for (int i = 0; i < 3; i++) {
        PStage s;
        setC(s, OP_MAD, tex(ch[i]), konst(OPR_RGB), i == 0 ? primary(OPR_RGB) : prev(OPR_RGB));
        constRgb(s, w[i]);
        if (!a) {
          setA(s, OP_MAD, tex(ch[i]), konst(OPR_A), i == 0 ? primary(OPR_A) : prev(OPR_A));   // alpha operands SRC_R / SRC_G / SRC_B exist too
          constA(s, w[i]);
        } else if (i == 0) {
          if (hasAlpha) setA(s, OP_REPLACE, tex(OPR_A)); else { setA(s, OP_REPLACE, konst(OPR_A)); constA(s, 1.0f); }
        } else {
          setA(s, OP_REPLACE, prev(OPR_A));
        }
        prog.stages.push_back(s);
      }
      break;
    }
    default: break;
  }
  return prog;
}

// ---------------------------------------------------------------- shaders
std::string efbBlitShaderSource() {
  std::string s;
  s += "; generated by tev2pica efbcopy.cpp -- EFB copy blit: position, texture coordinate, constant primary colour (the luma offset)\n";
  char b[160];
  std::snprintf(b, sizeof b, ".constf kPrim(%.17g, %.17g, %.17g, %.17g)\n", double(kEfbCopyPrimary), double(kEfbCopyPrimary), double(kEfbCopyPrimary), double(kEfbCopyPrimary));
  s += b;
  s += ".in inPos v0\n.in inTex v1\n.out outpos position\n.out outclr color\n.out outtc0 texcoord0.xy\n\n.proc main\n";
  s += "mov outpos, inPos\nmov outclr, kPrim\nmov outtc0, inTex\nend\n.end\n";
  return s;
}

std::string efbClearShaderSource() {
  std::string s;
  s += "; generated by tev2pica efbcopy.cpp -- copy-clear: clip position and colour straight through\n";
  s += ".in inPos v0\n.in inClr v1\n.out outpos position\n.out outclr color\n\n.proc main\n";
  s += "mov outpos, inPos\nmov outclr, inClr\nend\n.end\n";
  return s;
}

// ---------------------------------------------------------------- planner
namespace {

void addQuadIdx(std::vector<uint16_t>& idx, uint16_t base) {
  const uint16_t q[6] = {0, 1, 2, 0, 2, 3};
  for (uint16_t i : q) idx.push_back(uint16_t(base + i));
}

HwStage stageWords(const PStage& st) {
  Inputs none;
  return encodeStage(st, resolveConst(st, none, true));
}

}  // namespace

bool planTexturedMesh(C3dPlan& L, C3dShaderCache& shaders, TexEntry* tex, int maxLevel, const std::vector<uint8_t>& verts, const std::vector<uint16_t>& idx, int w, int h, std::string& why) {
  L.shader = shaders.get(efbBlitShaderSource());
  if (!L.shader->error.empty()) { L.ok = false; why = L.why = "mesh shader: " + L.shader->error; return false; }
  if (!tex || !tex->tex.ok) { L.ok = false; why = L.why = "mesh: no texture"; return false; }
  L.ok = true;
  L.attrs.count = 2;
  L.attrs.loader[0] = {0, ctru::FLOAT, 3};
  L.attrs.loader[1] = {1, ctru::FLOAT, 2};
  L.attrs.permutation = 0x10;
  L.attrs.stride = 20;
  const PicaProgram prog = efbCopyProgram(CopyProg::Color, true);
  const HwStage pass = {0x000F000Fu, 0u, 0u, 0xFFFFFFFFu, 0u};
  L.texenv.used = int(prog.stages.size());
  for (int i = 0; i < 6; i++) L.texenv.stage[i] = i < L.texenv.used ? stageWords(prog.stages[size_t(i)]) : pass;
  L.texenv.updateBuffer = 0; L.texenv.bufferColor = 0;
  C3dUnit& U = L.units[0];
  U.enabled = true; U.entry = tex;
  U.wrapS = U.wrapT = ctru::REPEAT;
  U.magFilter = U.minFilter = ctru::LINEAR;
  U.mipFilter = maxLevel > 0 ? ctru::LINEAR : ctru::NEAREST;
  U.minLevel = 0; U.maxLevel = uint8_t(std::max(0, maxLevel));
  U.lodBias = 0;
  C3dEffect& E = L.fx;
  E.depthTest = false; E.depthFunc = PF_ALWAYS;
  E.writeMask = uint8_t(ctru::WRITE_RED | ctru::WRITE_GREEN | ctru::WRITE_BLUE | ctru::WRITE_ALPHA);
  E.cull = ctru::CULL_NONE;
  L.scissor = PixelRect{0, 0, w, h};
  L.vertices = verts.data(); L.vertexBytes = uint32_t(verts.size());
  L.indices = idx.data(); L.indexCount = uint32_t(idx.size());
  c3dFinalize(L);
  return true;
}

EfbCopyPlan planEfbCopy(const GxCopy& c, const GxState& at, const FrameConfig& cfgIn, C3dShaderCache& shaders, TexEntry* mainView, const EfbCopyOptions& opt) {
  EfbCopyPlan R;
  FrameConfig cfg = cfgIn;
  cfg.resolve();
  R.toXfb = c.toXfb;
  R.ok = true;
  std::memcpy(R.clearColor, c.clearColor, 4);
  R.clearZ = c.clearZ;
  const bool hasAlpha = at.pixelFmt == 1;
  if (c.srcW < 1 || c.srcH < 1) { R.ok = false; R.why = "empty copy rectangle"; return R; }

  const double kx = double(cfg.outW) / cfg.efbW, ky = double(cfg.outH) / cfg.efbH;
  // (Claude v106) widescreen: full-viewport 3D / partial-width 2D draws are squeezed by s about the centre (widescreenXScale), so a
  // partial-width copy of the screen (Pictograph shutter, photo-judging peek) reads the squeezed region it shows. Full-width copies (DOF,
  // blur: drawn back full width, unsqueezed) and copies made under a partial viewport (render-to-texture, drawn unsqueezed) map 1:1.
  double sx = 1.0;
  if (cfg.widescreen && (c.srcX > 0 || c.srcX + c.srcW < cfg.efbW)) {
    const GxViewport& V = at.viewport;
    if (V.x <= 0.5f && V.x + V.width >= float(cfg.efbW) - 0.5f) sx = std::min(1.0, (4.0 / 3.0) / (double(cfg.outW) / double(cfg.outH)));
  }
  const double cx = cfg.outX + cfg.efbW * 0.5 * kx;
  const double lx0 = cx + (c.srcX - cfg.efbW * 0.5) * kx * sx, lx1 = cx + (c.srcX + c.srcW - cfg.efbW * 0.5) * kx * sx;
  const double ly0 = cfg.outY + c.srcY * ky, ly1 = cfg.outY + (c.srcY + c.srcH) * ky;
  R.srcLogicalX0 = int(std::floor(lx0)); R.srcLogicalX1 = int(std::ceil(lx1));
  R.srcLogicalY0 = int(std::floor(ly0)); R.srcLogicalY1 = int(std::ceil(ly1));

  // ================================================================ texture
  if (!c.toXfb) {
    const EfbCopyFormat F = efbCopyFormat(c);
    if (!F.known) {
      R.notes.push_back("copy to " + std::string(F.name) + ": " + F.note + "; no texture is produced (a binding of " + std::to_string(c.dstAddr) + " reads guest memory)");
    } else if (!mainView || !mainView->tex.ok) {
      R.notes.push_back("no view of the main target: the copy is not performed");
    } else if (!cfg.rotate) {
      R.notes.push_back("copies from an unrotated target are not implemented");
    } else {
      const int gxW = c.halfScale ? std::max(1, c.srcW / 2) : c.srcW, gxH = c.halfScale ? std::max(1, c.srcH / 2) : c.srcH;
      const int portW = std::max(1, int(std::floor(lx1 - lx0 + 0.5))), portH = std::max(1, int(std::floor(ly1 - ly0 + 0.5)));
      const int allocW = pow2ceil8(portW), allocH = pow2ceil8(portH);
      if (allocW > 1024 || allocH > 1024) {
        R.notes.push_back("copy of " + std::to_string(portW) + "x" + std::to_string(portH) + " target pixels exceeds the 1024 texture limit: not performed");
      } else {
        RenderedTexture& T = R.tex;
        T.addr = c.dstAddr;
        T.gxFmt = F.sampleFmt; T.gxW = gxW; T.gxH = gxH;
        T.guestBytes = efbCopyGuestBytes(F.sampleFmt, gxW, gxH);
        T.pfmt = F.pfmt;
        T.allocW = allocW; T.allocH = allocH; T.portW = portW; T.portH = portH;
        T.cost = size_t(allocW) * size_t(allocH) * size_t(bytesPerPixel(F.pfmt));
        if (!F.note.empty()) T.notes.push_back(F.note);
        if (portW != gxW || portH != gxH)
          T.notes.push_back("picture kept at the port's resolution (" + std::to_string(portW) + "x" + std::to_string(portH) + " texels for a " + std::to_string(gxW) + "x" + std::to_string(gxH) + " GX image)");
        auto aligned = [](double v) { return std::fabs(v - std::floor(v + 0.5)) < 1e-6; };
        if (!aligned(lx0) || !aligned(ly0) || !aligned(lx1 - lx0) || !aligned(ly1 - ly0))
          T.notes.push_back("copy rectangle edges fall between target pixels: the picture is resampled with nearest neighbour");
        if (c.srcX < 0 || c.srcY < 0 || c.srcX + c.srcW > cfg.efbW || c.srcY + c.srcH > cfg.efbH)
          T.notes.push_back("copy rectangle extends beyond the EFB: the outside reads the edge of the target");
        R.tex.cost = std::max<size_t>(R.tex.cost, 1);
        for (const std::string& n : T.notes) R.notes.push_back(n);

        // ---- geometry: cells of the allocation: [picture | right padding] x [picture | bottom padding]; padding repeats the picture's last texel row / column
        const double stepX = (lx1 - lx0) / portW, stepY = (ly1 - ly0) / portH;
        struct Axis { int p0, p1; bool pad; };
        std::vector<Axis> cols, rows;
        cols.push_back({0, portW, false}); if (allocW > portW) cols.push_back({portW, allocW, true});
        rows.push_back({0, portH, false}); if (allocH > portH) rows.push_back({portH, allocH, true});
        auto srcAt = [](const Axis& a, int p, double start, double step, int port) {
          return a.pad ? start + (port - 0.5) * step : start + p * step;
        };
        // (Claude v100) the view may be padded to a power of two (the console's sampler): normalise by the view texture, not the target
        const double viewW = (mainView && mainView->tex.width) ? double(mainView->tex.width) : double(cfg.targetH);
        const double viewH = (mainView && mainView->tex.height) ? double(mainView->tex.height) : double(cfg.targetW);
        uint16_t base = 0;
        for (const Axis& rw : rows) for (const Axis& cl : cols) {
          const int xs[4] = {cl.p0, cl.p1, cl.p1, cl.p0}, ys[4] = {rw.p0, rw.p0, rw.p1, rw.p1};
          for (int k = 0; k < 4; k++) {
            const double lx = srcAt(cl, xs[k], lx0, stepX, portW), ly = srcAt(rw, ys[k], ly0, stepY, portH);
            putf(R.blitVerts, float(-1.0 + 2.0 * xs[k] / allocW));
            putf(R.blitVerts, float(1.0 - 2.0 * ys[k] / allocH));
            putf(R.blitVerts, -0.5f);
            putf(R.blitVerts, float((cfg.targetH - ly) / viewW));  // framebuffer column X = targetH - 1 - ly
            putf(R.blitVerts, float(1.0 - lx / viewH));             // framebuffer row Y = lx, row 0 at v = 1 (PICA rows run bottom-up: translate.h flipTexT)
          }
          addQuadIdx(R.blitIdx, base);
          base = uint16_t(base + 4);
        }

        // ---- plan
        C3dPlan& L = R.blit;
        L.shader = shaders.get(efbBlitShaderSource());
        if (!L.shader->error.empty()) { L.ok = false; L.why = "blit shader: " + L.shader->error; R.notes.push_back(L.why); }
        else {
          L.ok = true;
          L.attrs.count = 2;
          L.attrs.loader[0] = {0, ctru::FLOAT, 3};
          L.attrs.loader[1] = {1, ctru::FLOAT, 2};
          L.attrs.permutation = 0x10;
          L.attrs.stride = 20;
          const PicaProgram prog = efbCopyProgram(F.prog, hasAlpha);
          const HwStage pass = {0x000F000Fu, 0u, 0u, 0xFFFFFFFFu, 0u};
          L.texenv.used = int(prog.stages.size());
          for (int i = 0; i < 6; i++) L.texenv.stage[i] = i < L.texenv.used ? stageWords(prog.stages[size_t(i)]) : pass;
          L.texenv.updateBuffer = 0; L.texenv.bufferColor = 0;
          C3dUnit& U = L.units[0];
          U.enabled = true; U.entry = mainView;
          U.wrapS = U.wrapT = ctru::CLAMP_TO_EDGE;
          U.magFilter = U.minFilter = U.mipFilter = ctru::NEAREST;
          C3dEffect& E = L.fx;
          E.depthTest = false; E.depthFunc = PF_ALWAYS;
          E.writeMask = uint8_t(ctru::WRITE_RED | ctru::WRITE_GREEN | ctru::WRITE_BLUE | ctru::WRITE_ALPHA);
          E.cull = ctru::CULL_NONE;
          L.scissor = PixelRect{0, 0, allocW, allocH};
          L.vertices = R.blitVerts.data(); L.vertexBytes = uint32_t(R.blitVerts.size());
          L.indices = R.blitIdx.data(); L.indexCount = uint32_t(R.blitIdx.size());
          c3dFinalize(L);
          R.toTexture = true;
        }
      }
    }
  }

  // ================================================================ clear
  if (c.clear) {
    R.clearsColor = at.colorUpdate;
    R.clearsAlpha = at.alphaUpdate && hasAlpha;
    R.clearsDepth = at.draw.z.update;
    if (!R.clearsColor && !R.clearsAlpha && !R.clearsDepth) {
      R.notes.push_back("copy-clear with colour, alpha and depth updates all disabled: nothing is written");
    } else {
      GxState S = at;
      S.viewport = GxViewport{0, 0, float(cfg.efbW), float(cfg.efbH), 0.0f, 1.0f};
      S.scissor = GxScissor{c.srcX, c.srcY, c.srcW, c.srcH};
      S.cullMode = 0;
      const bool wMode = opt.frameFogRange > 0;
      const PicaFrame F = mapFrame(S, cfg, wMode ? depthMapW(opt.frameFogRange) : depthMapZ());
      if (!F.ok || F.scissorWindow.empty()) {
        R.notes.push_back("copy-clear rectangle covers no target pixel: nothing is cleared");
      } else {
        const double zn = double(c.clearZ) / 16777215.0;
        const double w = wMode ? std::max(zn, 1e-3) * double(opt.frameFogRange) : 1.0;
        if (wMode && zn < 1e-3) R.notes.push_back("copy-clear depth below 1/1000 of the W-buffer range is written as 1/1000");
        if (wMode) R.notes.push_back("copy-clear in a W-buffer frame: the clear depth is read as a fraction of the fog range");
        const float rgba[4] = {c.clearColor[0] / 255.0f, c.clearColor[1] / 255.0f, c.clearColor[2] / 255.0f, c.clearColor[3] / 255.0f};
        const double nx[4] = {-1, 1, 1, -1}, ny[4] = {-1, -1, 1, 1};
        const float clearDepth = picaConstDepth(double(c.clearZ) / 16777215.0, c.clearZ >= 0xFFFFFFu);   // (Claude v107) reversed Z aware
        for (int k = 0; k < 4; k++) {
          double clip[4];
          const double nz = -0.5;   // (Claude v84) inside the clip volume; the depth written is the constant below
          for (int r = 0; r < 4; r++) clip[r] = double(F.post[r][0]) * nx[k] + double(F.post[r][1]) * ny[k] + double(F.post[r][2]) * nz + double(F.post[r][3]);
          if (wMode) { for (int r = 0; r < 2; r++) clip[r] *= w; clip[2] = -0.5 * w; clip[3] = w; }
          if (g_picaDepthFold) clip[2] = -double(clearDepth) * clip[3];   // (Claude v90) the Z map (0, clearDepth) folded: -z'/w = clearDepth
          for (int r = 0; r < 4; r++) putf(R.clearVerts, float(clip[r]));
          for (int r = 0; r < 4; r++) putf(R.clearVerts, rgba[r]);
        }
        addQuadIdx(R.clearIdx, 0);
        C3dPlan& L = R.clear;
        L.shader = shaders.get(efbClearShaderSource());
        if (!L.shader->error.empty()) { L.ok = false; L.why = "clear shader: " + L.shader->error; R.notes.push_back(L.why); }
        else {
          L.ok = true;
          L.attrs.count = 2;
          L.attrs.loader[0] = {0, ctru::FLOAT, 4};
          L.attrs.loader[1] = {1, ctru::FLOAT, 4};
          L.attrs.permutation = 0x10;
          L.attrs.stride = 32;
          PStage s;
          setC(s, OP_REPLACE, primary(OPR_RGB));
          setA(s, OP_REPLACE, primary(OPR_A));
          const HwStage pass = {0x000F000Fu, 0u, 0u, 0xFFFFFFFFu, 0u};
          L.texenv.used = 1;
          for (int i = 0; i < 6; i++) L.texenv.stage[i] = i == 0 ? stageWords(s) : pass;
          C3dEffect& E = L.fx;
          // (Claude v84) a constant depth: zn of the map's range, and a far clear above any geometry (kPicaDepthClearFar, see pipeline.h). Both maps wrote zn
          // before; the W map read it as a fraction of the frame range, which is the same value.
          E.zBuffer = true; E.zScale = g_picaDepthFold ? -1.0f : 0.0f; E.zOffset = g_picaDepthFold ? 0.0f : clearDepth;
          E.cull = ctru::CULL_NONE;
          E.depthTest = true; E.depthFunc = PF_ALWAYS;
          E.writeMask = uint8_t((R.clearsColor ? ctru::WRITE_RED | ctru::WRITE_GREEN | ctru::WRITE_BLUE : 0) | (R.clearsAlpha ? ctru::WRITE_ALPHA : 0) | (R.clearsDepth ? ctru::WRITE_DEPTH : 0));
          L.scissor = F.scissorWindow;
          L.vertices = R.clearVerts.data(); L.vertexBytes = uint32_t(R.clearVerts.size());
          L.indices = R.clearIdx.data(); L.indexCount = uint32_t(R.clearIdx.size());
          c3dFinalize(L);
          R.clears = true;
        }
      }
    }
  }
  return R;
}

}  // namespace t2p
