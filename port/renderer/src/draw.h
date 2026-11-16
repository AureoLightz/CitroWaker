// Draw-state compile step: everything the game configures for one draw (TEV, texgens + lighting channels, alpha test, blend, z mode, fog)
// -> everything the PICA needs (texenv program, vertex shader + attribute layout, texture-unit assignment, fixed-function state).
// Game-agnostic glue around translate.h (TEV), vtx.h (vertex pipeline) and pipeline.h (fixed-function state).
// Every approximation and every refusal is reported in `notes` / `blockers`; nothing is dropped silently.
#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "pipeline.h"
#include "translate.h"
#include "vtx.h"

namespace t2p {

// GX has texmaps 0..7. In a PICA unit assignment (PicaDraw::unitTexmap) this number names the multipass scratch target: unit 0 of a second pass samples what the first pass rendered.
constexpr int kScratchTexmap = 8;

struct GxDrawState {
  TevMaterial tev;                    // stage.texmap = GX texmap or -1, stage.texcoord = GX texcoord or -1, stage.rasChan 0 = COLOR0A0
  bool indirect = false;              // indirect texturing configured (dropped: the base texture is drawn unwarped; flagged)
  uint32_t indRaw[4] = {};            // (Claude v106 diag) BP 0x10..0x13 (TEV stages 0..3 indirect words), BP 0x27 in indRaw[3] high byte unused
  std::vector<std::string> importBlockers;   // things the GX->model conversion could not represent (compare ops, ...): passed through
  VtxSetup vtx;                       // color0 / alpha0 channels; texgen[i] = GX_TEXCOORDi. Attribute registers are assigned here, not by the caller
  int numPosMtx = 1;                  // position matrices the draw can index (1: no per-vertex index)
  GxAlphaCompare alpha;
  GxBlend blend;
  GxZMode z;
  GxFog fog;                          // fog.type 0 = off
  float projFar = 100000.0f;
  uint8_t texmapScaled = 0;
  bool projOrtho = false;             // (Claude v84) the projection is orthographic (w = 1): its far plane does not bound the W-buffer range           // bit m: the texture bound to GX texmap m fills only part of its PICA texture (texfit.h): its unit gets a texScale uniform
};

struct DrawAttr {                     // one vertex attribute the host must supply, in PICA input register order
  enum Kind { Pos, Nrm, Clr0, MtxIdx, Tex } kind = Pos;
  int index = 0;                      // Tex: GX texcoord attribute number 0..7
  int reg = 0;                        // PICA input register (= attribute loader index; the list is contiguous from 0)
};

// What a PicaDraw is in a multipass draw (multipass.cpp). A material that does not fit one set of PICA texenv stages (more than 6, or more than 3 texture units) is cut after GX stage
// `splitAt` and drawn twice over the same geometry: the first pass renders the stages before the cut into a scratch target (RGBA8, the size of the main target, its own depth buffer),
// the second pass starts with a stage that reads the scratch at the fragment's own pixel (texture unit 0, SCREEN texgen) and runs the rest with the draw's blend / alpha test / fog / depth.
enum class PassRole : uint8_t { Single, ScratchWrite, ScratchRead };

struct PicaDraw {
  Fidelity overall = Fidelity::Unsupported;
  std::vector<std::string> blockers;  // why overall is Unsupported
  std::vector<std::string> notes;     // every approximation
  std::vector<std::string> info;      // requirements for the host that are not approximations (RGBA target, W-buffer, ...)

  Translation tev;                    // texmap fields of the PICA program refer to PICA texture units
  VtxSetup vtx;                       // as handed to the generator (registers assigned)
  VtxShader vs;
  std::vector<DrawAttr> attrs;
  int numUnits = 0;                   // PICA texture units used
  int unitTexmap[3] = {-1, -1, -1};   // PICA unit u samples the texture bound to GX texmap unitTexmap[u] ...
  int unitTexcoord[3] = {-1, -1, -1}; // ... through GX texcoord unitTexcoord[u] (= PICA texcoord u, produced by texgen u of `vtx`)

  PicaAlphaTest alpha;
  PicaBlend blend;
  PicaDepth depth;
  PicaDepthMap depthMap;
  PicaFog fog;
  bool unitNeedAlpha[3] = {false, false, false};   // some stage reads this unit's alpha (lets texconv pick colour-only formats otherwise)
  // ---- multipass
  PassRole role = PassRole::Single;
  std::shared_ptr<const PicaDraw> pass2;   // ScratchWrite: the second pass (the draw to execute after this one, with the same geometry); null otherwise
  int splitAt = 0;                    // ScratchWrite: the GX stage the second pass starts with
  bool scratchDepth = false;          // ScratchWrite: the pass depth-tests against the scratch's own depth buffer (cleared to scratchDepthFar ? far : near before the pass)
  bool scratchDepthFar = true;
  uint64_t vsKey = 0;                 // hash of the vertex shader text: draws with equal keys share a shader
  int vsInstructions = 0, vsOpdescs = 0, vsUniformRegs = 0;   // as picasso would assemble it (limits: 512 / 128 / 96)
  // C3dShaderCache::get(const PicaDraw&) remembers its answer here (draws share one PicaDraw through the DrawCompileCache): the shader text is not
  // hashed and compared again for every draw. `cacheId` names the cache that answered (its shaders live as long as it does). A copy starts empty (it may get another text).
  struct ShaderMemo {
    uint64_t cacheId = 0;
    void* shader = nullptr;
    ShaderMemo() = default;
    ShaderMemo(const ShaderMemo&) {}
    ShaderMemo& operator=(const ShaderMemo&) { cacheId = 0; shader = nullptr; return *this; }
  };
  mutable ShaderMemo shaderMemo;
  // submit.cpp fillPacket remembers the texenv words it resolved for the last constant inputs (konst colours + TEV registers, compared bit for bit):
  // encodeStage(stage, resolveConst(stage, in)) depends on nothing else. Render-owner thread only, like shaderMemo. A copy starts empty.
  struct TexEnvMemo {
    bool valid = false;
    float in[7][4] = {};
    std::vector<HwStage> words;
    uint32_t updateBuffer = 0;
    TexEnvMemo() = default;
    TexEnvMemo(const TexEnvMemo&) {}
    TexEnvMemo& operator=(const TexEnvMemo&) { valid = false; return *this; }
  };
  mutable TexEnvMemo texenvMemo;
};

// A draw that does not fit one pass is split in two when Options::multipass allows (multipass.cpp): the result is then the first pass with `pass2` set. `overall` is the weaker of
// the two passes; the draw-state conditions under which the split is not exactly the single-pass picture (overlapping fragments of one draw, see multipass.cpp) are notes.
PicaDraw compileDraw(const GxDrawState&, const Options& = {});
// One pass only (no splitting): what compileDraw does before it considers a split.
PicaDraw compileDrawSingle(const GxDrawState&, const Options& = {});
// One pass, else two (Options::multipass), but no degrading: what compileDraw does before the approximate rung (approx.h).
PicaDraw compileDrawExact(const GxDrawState&, const Options& = {});

// Frame policy for fog. The PICA fog unit reads the depth buffer, so a frame that fogs anything needs a W-buffer for ALL its draws (z and w depths cannot be
// compared), with one range D for the whole frame. frameFogRange returns the D for a frame given the draw states it contains (the largest range any fogged
// draw asks for, so no fogged draw's ramp is cut off), or 0 if no draw fogs (the frame then uses the z map). Pass the result as Options::frameFogRange to
// compileDraw / buildDraw for every draw of that frame. Cost: a draw that wanted a small D gets coarser LUT cells (its PicaFog::maxErr255 says how much).
float frameFogRange(const GxDrawState* draws, size_t n);
class FrameFogRange {
 public:
  void add(const GxDrawState&);
  // (Claude v84) a W frame's range also covers the far plane of every perspective draw: the console's depth unit wraps values >= 1.0 to 0 (Azahar clamps),
  // so geometry beyond D would be drawn as the nearest thing on screen.
  float value() const { return range_ > 0 ? std::max(range_, far_) : 0.0f; }
 private:
  struct Seen { GxFog f; float projFar; };
  std::vector<Seen> seen_;
  float range_ = 0, far_ = 0;
};

}  // namespace t2p
