#include "multiplan.h"

#include <cstring>

#include "efbcopy.h"

namespace t2p {
namespace {

void putf(std::vector<uint8_t>& v, float x) { const size_t o = v.size(); v.resize(o + 4); std::memcpy(&v[o], &x, 4); }

}  // namespace

C3dMultiPlan buildMultiPlan(const DrawPacket& P, C3dShaderCache& shaders, TextureCache& tex, const GuestMem& mem, TexEntry* scratchView,bool diagnostics) {
  C3dMultiPlan M;
  if (!P.ok) { M.why = "the packet is not drawable"; return M; }
  if (!P.second || P.pica().role != PassRole::ScratchWrite || P.second->pica().role != PassRole::ScratchRead) { M.why = "not a two-pass packet"; return M; }
  M.first = buildPlan(P, shaders, tex, mem,nullptr,diagnostics);
  if (!M.first.ok) { M.why = "first pass: " + M.first.why; return M; }
  M.second = buildPlan(*P.second, shaders, tex, mem, scratchView,diagnostics);
  if (!M.second.ok) { M.why = "second pass: " + M.second.why; return M; }

  // The first pass rasterizes the same triangles as the second one or the read-back would fetch pixels the second pass does not draw (and the reverse): same cull mode, same scissor.
  if (M.first.fx.cull != M.second.fx.cull || M.first.scissor.x0 != M.second.scissor.x0 || M.first.scissor.y0 != M.second.scissor.y0 ||
      M.first.scissor.x1 != M.second.scissor.x1 || M.first.scissor.y1 != M.second.scissor.y1 || M.first.indexCount != M.second.indexCount) {
    M.why = "the two passes would not cover the same pixels (cull / scissor / index count differ)";
    return M;
  }

  // ---- the scratch depth clear: a quad over the whole target (the scissor cuts it to the draw's rectangle) writing depth only, a constant 1.0 (far) or 0.0 (near)
  if (P.pica().scratchDepth) {
    const bool far = P.pica().scratchDepthFar;
    const float px[4] = {-1, 1, 1, -1}, py[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 4; k++) {
      putf(M.clearVerts, px[k]); putf(M.clearVerts, py[k]); putf(M.clearVerts, g_picaDepthFold ? -picaConstDepth(0.0, far) : -0.5f); putf(M.clearVerts, 1.0f);   // (Claude v90) folded constant depth
      for (int c = 0; c < 4; c++) putf(M.clearVerts, 0.0f);
    }
    const uint16_t q[6] = {0, 1, 2, 0, 2, 3};
    for (uint16_t i : q) M.clearIdx.push_back(i);
    C3dPlan& L = M.clear;
    L.shader = shaders.get(efbClearShaderSource());
    if (!L.shader->error.empty()) { M.why = "scratch clear shader: " + L.shader->error; return M; }
    L.ok = true;
    L.attrs.count = 2;
    L.attrs.loader[0] = {0, ctru::FLOAT, 4};
    L.attrs.loader[1] = {1, ctru::FLOAT, 4};
    L.attrs.permutation = 0x10;
    L.attrs.stride = 32;
    PStage s;
    s.c.op = OP_REPLACE; s.c.arg[0] = PArg{P_PRIMARY, OPR_RGB};
    s.a.op = OP_REPLACE; s.a.arg[0] = PArg{P_PRIMARY, OPR_A};
    Inputs none;
    const HwStage pass = {0x000F000Fu, 0u, 0u, 0xFFFFFFFFu, 0u};
    L.texenv.used = 1;
    for (int i = 0; i < 6; i++) L.texenv.stage[i] = i == 0 ? encodeStage(s, resolveConst(s, none, true)) : pass;
    C3dEffect& E = L.fx;
    E.zBuffer = true; E.zScale = g_picaDepthFold ? -1.0f : 0.0f; E.zOffset = g_picaDepthFold ? 0.0f : (far ? kPicaDepthClearFar : 0.0f);   // (Claude v84) never 1.0: the console wraps it to 0   // depth = scale * z/w + offset: a constant
    E.cull = ctru::CULL_NONE;
    E.depthTest = true; E.depthFunc = PF_ALWAYS;
    E.writeMask = uint8_t(ctru::WRITE_DEPTH);
    L.scissor = M.first.scissor;
    L.vertices = M.clearVerts.data(); L.vertexBytes = uint32_t(M.clearVerts.size());
    L.indices = M.clearIdx.data(); L.indexCount = uint32_t(M.clearIdx.size());
    c3dFinalize(L);
    M.hasClear = true;
  }
  M.ok = true;
  return M;
}

}  // namespace t2p
