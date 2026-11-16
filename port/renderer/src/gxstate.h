#include "uniform_storage.h"
// GX register files (gxfifo.h) -> the state of one draw, in the shapes the translators consume (draw.h GxDrawState, vtx.h VtxUniforms)
// plus everything the renderer needs around it (texture units, viewport / scissor, constants, copies).
//   Bit layouts are the hardware's, as the game's own GX SDK writes them (tests/data/gx/st.bin is produced by that SDK code;
//   tests/test_gxstate.cpp decodes it and compares with the arguments the SDK was called with).
//   Game-agnostic. Everything the translators cannot represent is reported in GxState::notes or GxDrawState::importBlockers.
#pragma once
#include <string>
#include <vector>
#include <memory>

#include "draw.h"
#include "gxfifo.h"

namespace t2p {

// ---- texture unit (BP TX_SETMODE0/1, TX_SETIMAGE0/3, TX_SETTLUT of one of the 8 GX texmaps)
struct GxTexBinding {
  bool valid = false;           // the image registers of this unit were written
  uint32_t addr = 0;            // physical main-memory address of the image
  int width = 0, height = 0;
  int fmt = 0;                  // GxTexFmt (texconv.h); C4 / C8 / C14X2 use the palette selected by tlutOffset
  uint8_t wrapS = 0, wrapT = 0; // GX_CLAMP 0, GX_REPEAT 1, GX_MIRROR 2
  bool magLinear = false, minLinear = false;
  uint8_t mipMode = 0;          // 0 none, 1 nearest between levels, 2 linear between levels
  float lodBias = 0, minLod = 0, maxLod = 0;
  bool edgeLod = false, biasClamp = false;
  uint8_t aniso = 0;            // log2 of the anisotropy
  uint32_t tlutOffset = 0;      // TX_SETTLUT: TMEM offset of the palette (same units as GxTlutLoad::tmemOffset)
  int tlutFmt = 0;              // GxTlutFmt
  int mipLevels() const;        // levels stored behind `addr` (1 without mipmapping)
};

struct GxViewport { float x = 0, y = 0, width = 0, height = 0, nearZ = 0, farZ = 1; };   // EFB pixels, depth range in [0,1]
struct GxScissor { int x = 0, y = 0, width = 0, height = 0; };

struct GxProjection {
  bool ortho = false;
  float p[6] = {};              // XF 0x1020..0x1025 as GXSetProjection stores them
  float nearZ = 0, farZ = 0;    // recovered from the matrix (positive eye distances), 0 when degenerate
  float m[4][4] = {};           // full clip matrix (GX clip space: z/w = -1 at the near plane, 0 at the far plane)
};

struct GxCopy {                 // BP 0x52 EFB copy command with the registers it uses
  bool toXfb = false;           // false: texture copy
  bool clear = false;           // clear the EFB after the copy (colour / z from clearColor / clearZ)
  bool halfScale = false;       // GXSetTexCopyDst(..., mipmap = TRUE): 2x2 box filtered half size
  bool depth = false;           // PE control selects Z24: the copy reads depth
  bool intensity = false;       // BP 0x52 bit 15 (GXSetTexCopyDst sets it for I4 / I8 / IA4 / IA8 / YUVA8): the copy converts RGB to luma. With the
                                // format nibble below it names the GX copy format: nibble 0..3 + intensity = I4 I8 IA4 IA8; without it R4 (0) RA4 (2) RA8 (3);
                                // 4 RGB565, 5 RGB5A3, 6 RGBA8, 7 A8, 8 R8, 9 G8, 10 B8, 11 RG8, 12 GB8 (GX_CTF_*); depth copies reuse the nibble for Z formats
  int fmt = 0;                  // copy format nibble (BP 0x52 bit 3 and bits 4..6, as GXSetTexCopyDst writes them)
  int srcX = 0, srcY = 0, srcW = 0, srcH = 0;
  uint32_t dstAddr = 0;         // physical address
  uint32_t dstStrideBytes = 0;  // row stride (XFB) / bytes per row of tiles (texture)
  uint8_t clearColor[4] = {};   // r g b a
  uint32_t clearZ = 0;          // 24 bit
};

struct GxTlutLoad {             // BP 0x64 / 0x65: palette load into TMEM
  uint32_t srcAddr = 0;         // physical address of the entries
  uint32_t tmemOffset = 0;      // destination (units of TX_SETTLUT.offset)
  uint32_t entries = 0;         // 16 entries per count unit
};

struct GxState {
  GxDrawState draw;             // TEV, vertex channels + texgens, alpha test, blend, z mode, fog, projection far plane
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
  // Immutable uniform blocks often repeat across a model's material draws.
  // Sharing their exact bytes removes kilobytes from every recorded event.
  std::shared_ptr<const VtxUniforms> uniformBlock;
  std::shared_ptr<const CompactUniforms> uniformSections;
#else
  VtxUniforms uniforms;
#endif
  VtxUniformView uniformView()const{
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
   if(uniformBlock)return uniformViewOf(*uniformBlock);
   if(uniformSections)return uniformSections->view();
#endif
   return uniformViewOf(vertexUniforms());
  }
  const VtxUniforms& vertexUniforms() const {
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
    static const VtxUniforms identity;
    return uniformBlock ? *uniformBlock : uniformSections ? uniformSections->get() : identity;
#else
    return uniforms;
#endif
  }
  Rgba kcolor[4];               // KCOLOR0..3, 0..1
  Rgba tevReg[4];               // initial PREV, REG0..2 (S11 / 255: may leave 0..1)
  GxTexBinding tex[8];
  GxViewport viewport;
  GxScissor scissor;
  GxProjection proj;
  uint8_t cullMode = 2;         // GXCullMode: NONE 0, FRONT 1, BACK 2, ALL 3
  bool colorUpdate = true, alphaUpdate = true, dither = false;
  bool dstAlphaEnable = false;
  uint8_t dstAlpha = 0;
  uint8_t pixelFmt = 0;         // PE control: RGB8_Z24 0, RGBA6_Z24 1, RGB565_Z16 2, Z24 3, ...
  uint8_t zFmt16 = 0;
  bool zCompBeforeTex = false;  // early depth test
  int numTexGens = 0, numChans = 0, numTevStages = 0, numIndStages = 0;
  int curPosMtx = 0;            // matrix number k used when the vertex stream has no matrix index
  int uniformPosSel = 0;        // (Claude) the position / normal matrix copied to slot 0 of the uniforms (0: none): with the registers, all the uniforms depend on
  std::vector<std::string> notes;   // everything that could not be represented (not a TEV blocker; see draw.importBlockers)
};

// Decodes the state for a draw. `draw` supplies the vertex descriptor and the matrix indices that occur (nullptr: use the register
// state only and assume one position matrix).
// notes=false: skip the diagnostic GxState::notes (exact otherwise). knownMaxPm: maxPosMtx(*draw) when the caller tracks it (the batch's position matrix indices
// are then not rescanned; required for a retained draw, whose vertices are not in *draw); kScanPosMtx: scan draw->v.pmIdx.
constexpr int kScanPosMtx = -1000;
GxState decodeState(const GxRegs&, const GxDraw* draw = nullptr, bool notes = true, int knownMaxPm = kScanPosMtx);
// The same into S, which must be default-constructed (a fresh GxState): saves the temporary and its copy (3 KB) per batch.
// Per-section memos of decodeStateInto (TEV stages, texgens, texture bindings): each section is a pure function of a few register words; the words the
// section read last time (per thread) are compared bit for bit and on equality its previous output is copied instead of decoded again. Exact; on by default
// (false: always decode, for A/B measurements).
void setStateDecodeMemo(bool on);
void decodeStateInto(GxState& S, const GxRegs&, const GxDraw* draw = nullptr, bool notes = true, int knownMaxPm = kScanPosMtx);
// (Claude) Incremental form. `prev` is a state decodeState / decodeStateInto produced earlier from the same GxRegs object, and `dirty` the union
// of GxFifo::takeStateDirty() since then (gxfifo.h kStateDirty*). S becomes what decodeStateInto(S, r, draw, false, knownMaxPm) would make of a
// fresh state: sections whose registers did not change are copied from prev, the others decoded again. Needs notes == false (the notes of a
// section depend on the draw's vertex descriptor); with notes, kStateDirtyOther or kStateDirtyAll it is the full decode.
void decodeStateIncremental(GxState& S, const GxRegs&, const GxDraw* draw, bool notes, int knownMaxPm, uint32_t dirty, const GxState* prev);


// Exact static material template plus dynamic state from the preceding live draw.
// Unlike marking every uniform dirty, this preserves valid clean uniform rows.
void decodeStateTemplate(GxState&,const GxRegs&,const GxDraw*,int knownMaxPm,uint32_t dirty,const GxState* previous,const GxState& material);

GxCopy decodeCopy(const GxRegs&);        // call from GxSink::onCopy
GxTlutLoad decodeTlutLoad(const GxRegs&);   // call from GxSink::onTlutLoad

}  // namespace t2p
