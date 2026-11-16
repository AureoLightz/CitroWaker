#include "material_vector.h"
// Vertex side: GX transform unit (XF) -> PICA200 vertex shader.
//   * VtxSetup      what the game configured: lighting channel 0 (color + alpha halves), texgens, attribute layout.
//   * evalVertex    reference implementation of the GX behaviour (Dolphin's hardware-shader flavour: integer
//                   lighting accumulator with round(), (mat * (lacc + (lacc >> 7))) >> 8, normalised eye normal).
//   * genVertexShader  emits a picasso (.vsh) program computing the same thing in float registers, plus the
//                   uniform / attribute contract the host has to honour.
// Game-agnostic. Channel 1 (COLOR1A1) is not generated: the census shows no TEV stage of the game reads it.
//
// Host contract of the generated shader (uniform names are exported by picasso; set them by name):
//   projMtx[4]        rows of the 4x4 projection (GX clip space as-is: z/w in [-1,0]; apply the screen tilt on the host)
//   posMtx[3*N]       N model-view matrices, 3 rows (vec4) each, row-major, as GXLoadPosMtxImm
//   nrmMtx[3*N]       N normal matrices, 3 rows each (the xyz of each row is used), as GXLoadNrmMtxImm
//   matColor, ambColor  channel-0 register colors, rgba as integers 0..255 (rgb: color half, a: alpha half)
//   light[5*L]        per used light (slot order = ascending GX light index, see VtxShader::lightSlot):
//                     +0 color rgba 0..255, +1 position (eye space), +2 direction (as GXInitLightDir stores it:
//                     already negated), +3 cosatt (a0,a1,a2), +4 distatt (k0,k1,k2)
//   texMtx[3*T]       3x4 texture matrices, 3 rows each, slot j = j-th texgen with a non-identity matrix
//   dbgScale          (debug builds only) scale, bias for the debug color output
// Vertex attributes (PICA input registers are chosen by VtxSetup::in*): position xyz float; normal xyz float;
// color0 as 4 x UNSIGNED_BYTE (PICA turns them into 0..255 floats, which is what the integer lighting math needs);
// texcoords float; matrix index as UNSIGNED_BYTE holding 3*index (GX's PNMTXIDX convention).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "picaasm.h"

namespace t2p {

struct VtxChan {            // one half (color or alpha) of XF channel 0
  uint8_t enable = 0;       // lighting enabled
  uint8_t matSrc = 0;       // GX_SRC_REG 0 / GX_SRC_VTX 1
  uint8_t lightMask = 0;    // bit i = GX_LIGHT_i
  uint8_t diffuse = 2;      // GX_DF_NONE 0 / SIGN 1 / CLAMP 2
  uint8_t attn = 2;         // GX_AF_SPEC 0 / SPOT 1 / NONE 2
  uint8_t ambSrc = 0;       // GX_SRC_REG 0 / GX_SRC_VTX 1
};

struct VtxTexGen {
  uint8_t type = 1;         // GX_TG_MTX3x4 0, MTX2x4 1, SRTG 10 (bump/emboss types are not supported); 20 = SCREEN (not a GX type: the multipass second pass reads the first pass's
                            // result at its own pixel; unit 0 only): (s, t, q) = (0.5 (x + w), 0.5 (y + w), w) of the clip position, so s / q, t / q are the pixel's position in
                            // the target in the GPU's own layout; no texScale, no flip
  uint8_t src = 4;          // GX_TG_POS 0, NRM 1, TEX0..TEX7 = 4..11, COLOR0 19, COLOR1 20
  uint8_t mtx = 60;         // GX_IDENTITY 60, GX_TEXMTX0..9 = 30 + 3k
  bool normalize = false;   // dual-transform: not supported (flagged)
  uint8_t postMtx = 125;    // GX_PTIDENTITY; anything else is not supported (flagged)
};

struct VtxSetup {
  VtxChan color0, alpha0;
  bool needColor = true;                  // false: the TEV never reads the rasterized color (and no texgen takes it)
  MaterialVector<VtxTexGen> texgen;          // texgen[i] feeds PICA texcoord i (at most 3)
  int numPosMtx = 1;                      // size of posMtx/nrmMtx arrays (1: no per-vertex matrix index)
  // PICA input register of each attribute, -1 = absent
  int inPos = 0, inNrm = -1, inClr0 = -1, inMtxIdx = -1;
  int inTex[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  // PICA samples textures upside down relative to GX (memory row 0 is at v = 1, measured on Azahar by hwcheck4): with flipT every texcoord
  // output is (s, q - t), i.e. t -> 1 - t after the projective divide. Texture data is uploaded in GX row order.
  bool flipT = false;
  // Units whose texture does not fill its PICA texture (padded / tiled by texmap.h) multiply (s, t) by a per-unit uniform before the flip:
  // s' = s * k.x, t' = q - t * k.y. Bit u = PICA texcoord u.
  uint8_t texScaleMask = 0;
  // debug builds for hwcheck3
  int dbgClipReg = -1;                    // >=0: clip position comes straight from this input register
  int dbgTc = -1;                         // >=0: color output = dbgScale.x * texcoord (s,t,q) + dbgScale.y
};

struct VtxShader {
  bool ok = false;
  std::string source;                     // picasso text
  std::string error;
  std::vector<std::string> notes;         // every approximation / unsupported feature, one line each
  bool exact = true;                      // false if any note is an approximation
  int lightSlot[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // light[5*slot ...] holds GX light i
  int numLights = 0;
  int texMtxSlot[3] = {-1, -1, -1};       // texMtx[3*slot ...]; -1 = identity (no uniform)
  int numTexMtx = 0;
  int texScaleSlot[3] = {-1, -1, -1};     // texScale[slot] = (kx, ky); -1 = unit not scaled (no uniform)
  int numTexScale = 0;
  bool tc0Projective = false;             // texcoord0w carries q: enable projection mode on texture unit 0
  bool usesNormal = false;                // lighting runs (needs nrmMtx and the normal attribute)
  // which input attributes the shader declares (so the host only has to supply these)
  bool usesPos = false, usesNrmAttr = false, usesClrAttr = false, usesMtxIdxAttr = false, usesTexAttr[8] = {};
  int uniformRegs = 0;                    // .fvec registers used (c0..); plus the .constf at the top
};
VtxShader genVertexShader(const VtxSetup&);

// ---------------------------------------------------------------- reference evaluator
struct VtxLight { float color[4] = {0, 0, 0, 0}, pos[3] = {0, 0, 0}, dir[3] = {0, 0, 1}, cosatt[3] = {1, 0, 0}, distatt[3] = {1, 0, 0}; };
struct VtxUniforms {
  float proj[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  float pos[10][3][4] = {};               // posMtx index -> 3 rows
  float nrm[10][3][4] = {};
  float matColor[4] = {255, 255, 255, 255}, ambColor[4] = {0, 0, 0, 255};
  VtxLight light[8];
  float texMtx[10][3][4] = {};            // GX_TEXMTX k -> 3 rows
  float texScale[3][2] = {{1, 1}, {1, 1}, {1, 1}};   // per PICA texcoord: image coordinate -> texture coordinate scale (only read for units in VtxSetup::texScaleMask)
};
// Private receiving-API prototype: fixed array references retain sizeof/index semantics.
struct VtxUniformView {
 const float (&proj)[4][4];const float (*pos)[3][4];const float (*nrm)[3][4];
 const float (&matColor)[4];const float (&ambColor)[4];const VtxLight (&light)[8];
 const float (&texMtx)[10][3][4];const float (&texScale)[3][2];
 int poseCount=10;
};
inline VtxUniformView uniformViewOf(const VtxUniforms& u){return {u.proj,u.pos,u.nrm,u.matColor,u.ambColor,u.light,u.texMtx,u.texScale};}
struct VtxInput {
  float pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, clr0[4] = {255, 255, 255, 255}, tex[8][2] = {};
  int mtxIdx = 0;                         // matrix number (shader attribute value is 3 * mtxIdx)
};
struct VtxOutput {
  float clip[4] = {0, 0, 0, 1};
  float color[4] = {1, 1, 1, 1};          // 0..1 (k / 255)
  float tc[3][3] = {};                    // s, t, q per texgen
};
VtxOutput evalVertex(const VtxSetup&, const VtxUniforms&, const VtxInput&);

// Packs GX-side state into the generated shader's uniform arrays / input registers (what the host does per draw).
// `shader` supplies the light and texture-matrix slot assignment. Keys are the uniform names of the host contract above.
// Fixed host-contract rows; reused by the renderer, with no per-draw map lookup.
struct PackedUniformRows { PVec rows[40]; int count = 0; };
struct PackedUniforms { PackedUniformRows arrays[8]; };
int packedUniformSlot(const std::string& name);
void packUniformsPacked(PackedUniforms&, const VtxSetup&, const VtxShader&, const VtxUniforms&, const float proj[4][4], const float scale[3][2]);
void packUniformsPacked(PackedUniforms&, const VtxSetup&, const VtxShader&, const VtxUniformView&, const float proj[4][4], const float scale[3][2]);
using UniformMap = std::map<std::string, std::vector<PVec>>;
UniformMap packUniforms(const VtxSetup&, const VtxShader&, const VtxUniforms&);
// Same values written into an existing map: keys it already has keep their nodes and vector capacity (no allocations per draw once warm).
void packUniformsInto(UniformMap& m, const VtxSetup&, const VtxShader&, const VtxUniforms&);
void packInputs(const VtxSetup&, const VtxInput&, PVec in[16]);

}  // namespace t2p
