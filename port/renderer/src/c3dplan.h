// Draw plan: one DrawPacket (submit.h) -> everything a citro3d executor has to do for that draw, as plain data (no GPU calls, no libctru).
//
// The executor (src/backend3ds.cpp, 3DS only) replays a plan one-to-one:
//   shader      C3D_BindProgram of the SHBIN built by picaenc (cached per generated shader text)
//   attributes  AttrInfo_AddLoader per loader + BufInfo_Add(stride, permutation); the vertex / index bytes are borrowed from the packet
//   uniforms    C3D_FVUnifSet rows at float uniform register `id`
//   texenv      C3D_SetTexEnv words 0..5 (unused stages are pass-through), C3D_TexEnvBufUpdate, C3D_TexEnvBufColor
//   units       C3D_TexBind + sampler state of the cached texture
//   effect      C3D_DepthMap / CullFace / DepthTest / AlphaTest / AlphaBlend or ColorLogicOp / Fog*
//   scissor     C3D_SetScissor
// Values use libctru's numbering (GPU_* enums); the executor static_asserts the ones it relies on.
// Splitting the work this way keeps every decision (register ids, loader formats, write masks, sampler bits, fog mode) testable on the host: the tests run the
// plan's own uniform rows through the SHBIN executor (tests/pica_exec.h) and compare with the GX reference evaluator.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "frame.h"
#include "picaenc.h"
#include "submit.h"
#include "texcache.h"

namespace t2p {

// ---------------------------------------------------------------- shaders
struct C3dShader {
  uint64_t key = 0;                       // hash of the shader text
  std::string source;                     // the picasso text this was built from
  PicaAsmResult bin;                      // bin.ok == false: the text did not assemble (bin.error says why); the plan is refused
  struct Uni { int id = 0; int size = 0; int packedSlot = -1; const std::string* name = nullptr; };   // (Claude v122) name: internUniformName
  std::map<std::string, Uni> uniforms;    // float vector uniforms: first register (c0 = 0, the id C3D_FVUnifSet takes) and number of rows
  std::vector<std::pair<std::string, Uni>> uniformsById;   // the same, ordered by register (the order a plan lists them in)
  uint16_t inputMask = 0;                 // input registers the shader reads
  std::string error;                      // non-empty: the shader cannot be used
  uintptr_t user = 0;                     // the host's loaded program (DVLB + shaderProgram_s); 0 until the executor loads it
};

class C3dShaderCache {
 public:
  // Assembles on the first sight of a shader text. Failures are cached too (C3dShader::error), so a bad text is not re-assembled for every draw.
  C3dShader* get(const PicaDraw&);
  C3dShader* get(const std::string& source);   // by text (shaders the port writes itself: EFB copy / clear)
  size_t size() const { return count_; }
  template <class F> void forEach(F f) { for (auto& kv : byKey_) for (auto& s : kv.second) f(*s); }
 private:
  std::map<uint64_t, std::vector<std::unique_ptr<C3dShader>>> byKey_;   // chained: the key is a hash, the text decides
  size_t count_ = 0;
  uint64_t id_ = nextId();                // PicaDraw::shaderCacheId of draws this cache answered
  static uint64_t nextId() { static uint64_t n = 0; return ++n; }
};

// ---------------------------------------------------------------- the plan
namespace ctru {   // libctru numbering used in plans
enum : int { BYTE = 0, UNSIGNED_BYTE = 1, SHORT = 2, FLOAT = 3 };                 // GPU_FORMATS
enum : int { CLAMP_TO_EDGE = 0, REPEAT = 2, MIRRORED_REPEAT = 3 };                // GPU_TEXTURE_WRAP_PARAM
enum : int { NEAREST = 0, LINEAR = 1 };                                           // GPU_TEXTURE_FILTER_PARAM
enum : int { CULL_NONE = 0, CULL_FRONT_CCW = 1, CULL_BACK_CCW = 2 };              // GPU_CULLMODE
enum : int { WRITE_RED = 1, WRITE_GREEN = 2, WRITE_BLUE = 4, WRITE_ALPHA = 8, WRITE_DEPTH = 0x10 };   // GPU_WRITEMASK
enum : int { NO_FOG = 0, FOG = 5 };                                               // GPU_FOGMODE
}  // namespace ctru

struct C3dLoader { int reg = 0, format = ctru::FLOAT, count = 0; };   // AttrInfo_AddLoader(regId, format, count)

struct C3dAttrs {
  C3dLoader loader[12];
  int count = 0;
  uint64_t permutation = 0;       // BufInfo_Add: attribute id i of the buffer in nibble i
  uint32_t stride = 0;
  bool planar = false;            // (Claude v77) one buffer per attribute: attribute i at byte offset[i] of the vertices, element size size[i]
  uint32_t offset[12] = {}, size[12] = {};
};

// (Claude v122) uniform names are interned (one string per distinct name, never freed): plans copy a pointer, not a string, per uniform per draw
const std::string* internUniformName(const std::string& name);
struct C3dUniform {
  const std::string* name = nullptr;   // interned (internUniformName): equal names have equal pointers
  int id = 0;                     // float uniform register (C3D_FVUnifSet id)
  const PVec* rows = nullptr;     // borrowed from the packet
  int count = 0;
};

struct C3dTexEnv {
  HwStage stage[6];               // C3D_TexEnv raw words {srcRgb | srcAlpha << 16, opAll, funcRgb | funcAlpha << 16, color, scaleRgb | scaleAlpha << 16}
  int used = 0;                   // stages the translator produced; the others pass PREVIOUS through
  uint32_t updateBuffer = 0;      // GPUREG_TEXENV_UPDATE_BUFFER bits 8..15: rgb mask << 8 | alpha mask << 12
  uint32_t bufferColor = 0;       // C3D_TexEnvBufColor: the combiner buffer starts at 0
};

struct C3dUnit {
  bool enabled = false;
  TexEntry* entry = nullptr;      // the cached, converted texture (entry->user = the host's C3D_Tex)
  bool projective = false;        // unit 0 divides by q (texture type PROJECTION)
  uint8_t wrapS = ctru::CLAMP_TO_EDGE, wrapT = ctru::CLAMP_TO_EDGE;
  uint8_t magFilter = ctru::NEAREST, minFilter = ctru::NEAREST, mipFilter = ctru::NEAREST;
  uint8_t minLevel = 0, maxLevel = 0;   // LOD clamp in levels of the stored chain
  uint16_t lodBias = 0;           // register field (13 bit two's complement, 1/256 units), computed like C3D_TexSetLodBias
};

struct C3dEffect {
  bool zBuffer = true; float zScale = 1, zOffset = 1;     // C3D_DepthMap
  uint8_t cull = ctru::CULL_NONE;                         // C3D_CullFace
  bool depthTest = true; uint8_t depthFunc = PF_LEQUAL; uint8_t writeMask = 0x1F;   // C3D_DepthTest(enable, func, GPU_WRITEMASK)
  bool alphaTest = false; uint8_t alphaFunc = PF_ALWAYS; uint8_t alphaRef = 0;       // C3D_AlphaTest
  bool logicOp = false; uint8_t logic = 3;                // C3D_ColorLogicOp, otherwise C3D_AlphaBlend with:
  uint8_t eqRgb = 0, eqAlpha = 0, srcRgb = 1, dstRgb = 0, srcAlpha = 1, dstAlpha = 0;
  uint32_t blendColor = 0;                                // C3D_BlendingColor (GX has no blend constant)
  bool fog = false; uint32_t fogColor = 0;                // C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false) + C3D_FogColor + lut
};

struct C3dPlan {
  bool ok = false;                // false: the draw is not executed (why)
  std::string why;
  C3dShader* shader = nullptr;
  C3dAttrs attrs;
  std::vector<C3dUniform> uniforms;
  C3dTexEnv texenv;
  C3dUnit units[3];
  C3dEffect fx;
  uint32_t fogLut[128] = {};      // C3D_FogLut::data, meaningful when fx.fog
  PixelRect scissor;              // GPU window pixels, x1 / y1 exclusive: C3D_SetScissor(GPU_SCISSOR_NORMAL, x0, y0, x1, y1)
  const uint8_t* vertices = nullptr; uint32_t vertexBytes = 0;   // borrowed from the packet: copy to linear memory
  const uint16_t* indices = nullptr; uint32_t indexCount = 0;
  uint64_t hashAttrs = 0, hashTexEnv = 0, hashEffect = 0;        // equal hash = equal state: the executor skips re-sending it
  bool linearGeometry = false;     // (Claude v73) vertices / indices already in flushed linear memory that outlives the GPU work (draw reuse): no arena copy
  std::vector<PVec> ownedRows;                                   // uniform rows the plan had to change (texture scale of rendered copies); C3dUniform::rows may point here
  std::vector<std::string> notes;                                // what the plan itself approximates or ignores
};

// ---------------------------------------------------------------- plan templates (optional, off unless a caller passes a cache)
// (Claude Opus, 2026-11-11) Most draws of a frame repeat a plan whose immutable part (attribute loaders, texenv words, fixed-function effect + fog LUT,
// the three state hashes and the uniform DESCRIPTORS) was already built for an earlier draw. A C3dPlanTemplateCache keeps that part, keyed exactly; a hit
// still runs, in the baseline order: packet / index / scissor checks, shaders.get, vertex size + index range, per-uniform packed row count, texenv checks,
// the SAME texture-unit code (one TextureCache::get per unit, same order) and the SAME rendered-copy texScale patch on the current rows. Only then the stored
// final texenv / effect / fog LUT / hashes are copied. Geometry and uniform row pointers always come from the current packet.
// Used only when diagnostics == false, the packet uses packed uniforms for every declared uniform, and it has a live compiled-draw owner (DrawPacket::picaOwner);
// otherwise buildPlanInto is the unchanged baseline. Only successfully built plans are stored.
// Contract: (1) a PicaDraw reached through a packet's owner is IMMUTABLE while alive (DrawCompileCache output; never picaMut() on such a packet);
// (2) one thread, not re-entrant; (3) clear() whenever the C3dShaderCache the plans use is replaced.
// Identity: the owner's control block (weak, never extends a PicaDraw's life) plus its address; an evicted/expired PicaDraw can never match a live one.
struct C3dPlanTemplateStats {
  uint64_t lookups = 0, hits = 0, misses = 0, inserts = 0, evictions = 0, expired = 0, rejected = 0, bypass = 0, shaderMismatch = 0;
  size_t entries = 0, bytes = 0, budget = 0;   // bytes: entries (incl. keys, descriptors, LUTs, list/map nodes) + bucket array + fixed object
};
class C3dPlanTemplateCache {
 public:
  explicit C3dPlanTemplateCache(size_t budgetBytes = 256u << 10);
  ~C3dPlanTemplateCache();
  C3dPlanTemplateCache(const C3dPlanTemplateCache&) = delete;
  C3dPlanTemplateCache& operator=(const C3dPlanTemplateCache&) = delete;
  void clear();                        // drops every entry (statistics kept)
  void setBudget(size_t budgetBytes);  // evicts down to the new budget
  void purgeExpired();                 // drops entries whose PicaDraw no longer exists (O(entries); e.g. once per frame)
  C3dPlanTemplateStats stats() const;
  struct Impl;
  Impl& impl() { return *impl_; }
 private:
  std::unique_ptr<Impl> impl_;
};

// `tex` and `mem` resolve the units' texture bindings (conversion happens here on a cache miss). The plan borrows from `P`, the shader cache and the texture cache.
// `scratchView`: the texture entry through which the multipass scratch target's colour buffer is sampled (a unit with SubmitUnit::scratch uses it; see multiplan.h).
// `templates`: optional plan-template cache (see above); nullptr = the baseline.
C3dPlan buildPlan(const DrawPacket& P, C3dShaderCache& shaders, TextureCache& tex, const GuestMem& mem, TexEntry* scratchView = nullptr, bool diagnostics = true,
                  C3dPlanTemplateCache* templates = nullptr);

// Fills the three state hashes (attributes, texenv, effect) of a plan whose fields are set; buildPlan does it at the end, planners that build plans by hand call it too.
C3dPlan& buildPlanInto(C3dPlan& out, const DrawPacket&, C3dShaderCache&, TextureCache&, const GuestMem&, TexEntry* scratchView = nullptr, bool diagnostics = true,
                       C3dPlanTemplateCache* templates = nullptr);

void c3dFinalize(C3dPlan& L);

// (Claude v87) plan delta: L holds the successful full plan of the previous event of the same frame, P is the packet of the next event made from the SAME
// compiled draw (same shader, attributes, texture bindings, effects pipeline) whose GX state differs only in TEV registers, matrices, viewport/scissor or
// lights. Every step of buildPlanInto that reads such state runs again, unchanged: geometry pointers, uniform rows (packed, same declarations), the
// rendered-copy texScale patch, texenv words, effects (depth map, cull, fog, dst-alpha stage) and c3dFinalize. Kept: shader, attribute loaders, texture
// units (same bindings, same frame). False: build it fully (L may be partly changed; buildPlanInto resets it).
bool buildPlanDelta(C3dPlan& L, const DrawPacket& P, bool diagnostics);

// float vector uniform register (C3D_FVUnifSet id) of a picasso position: pos - 0x20 (libctru: DVLE_GetUniformRegister = startReg - 0x10 with startReg = pos - 0x10)
inline int c3dUniformId(int picassoPos) { return picassoPos - 0x20; }

// C3D_TexSetLodBias arithmetic
uint16_t c3dLodBias(float bias);

}  // namespace t2p
