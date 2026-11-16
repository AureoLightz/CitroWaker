// One decoded GX draw -> one self-contained PICA draw packet: compiled state (draw.h), the vertex buffer in the PICA attribute layout,
// the 16-bit index list, the shader uniforms, the texenv words with the runtime constants (KCOLOR / initial TEV registers) resolved,
// the texture units with their GX bindings, and the pass-through frame state (viewport, scissor, cull, masks).
// This is the last game-agnostic step before a backend (citro3d on the 3DS, or any other consumer) touches the packet.
// Nothing here owns GPU resources: the packet is plain data.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "draw.h"
#include "frame.h"
#include "gxfifo.h"
#include "gxstate.h"

namespace t2p {
extern bool g_packPlanar;        // (Claude v77) one array per attribute (same bytes per attribute, same total size); the backend binds a GPU buffer per attribute
extern bool g_packVertexMajor;   // (Claude) pack each vertex in one pass (exact; for the ARM11 write buffer). Default false.

class DrawCompileCache;
namespace matrixbatch { class Group; }

// Packed vertex / index buffers of retained geometry (gxretain.h) kept across frames: a retained batch drawn again with the same attribute layout swaps them into
// its packet instead of packing (fillPacket) again. Identity is exact: the geometry id, the stride and every attribute of the layout are compared.
// Entries not used during a frame are dropped by endFrame().
class PackedGeometryCache {
 public:
  struct Entry {
    uint64_t geoId = 0; uint32_t stride = 0; std::vector<uint32_t> sig;
    std::vector<uint8_t> vertices; std::vector<uint16_t> indices;
    bool missingNrm = false, missingClr = false, indicesChecked = false;
    uint64_t frame = 0;
  };
  Entry* find(uint64_t key, uint64_t geoId, uint32_t stride, const std::vector<uint32_t>& sig);
  Entry* insert(uint64_t key);   // nullptr when over budget
  uint64_t frame() const { return frame_; }
  void endFrame();
  void clear() { map_.clear(); bytes_ = 0; }
  void setBudget(size_t b) { budget_ = b; }
  size_t bytes() const { return bytes_; }
  size_t entries() const { return map_.size(); }
 private:
  std::unordered_map<uint64_t, Entry> map_;
  uint64_t frame_ = 1;
  size_t budget_ = 6u << 20, bytes_ = 0;
};

struct SubmitAttr {
  DrawAttr::Kind kind = DrawAttr::Pos;
  int index = 0;              // Tex: GX texcoord attribute number
  int reg = 0;                // PICA input register (attribute loader index)
  int components = 3;
  bool ubyte = false;         // UNSIGNED_BYTE components (colour, matrix index); otherwise FLOAT
  uint32_t offset = 0;        // byte offset inside a vertex (4 byte aligned)
};

struct SubmitUnit {           // PICA texture unit u
  int texmap = -1;            // GX texmap sampled
  int texcoord = -1;          // GX texcoord (= PICA texcoord u)
  bool scratch = false;       // the multipass scratch target (draw.h kScratchTexmap): no GX binding; the backend supplies the texture
  bool needAlpha = false;     // some stage reads this unit's alpha (lets texconv pick colour-only formats otherwise)
  GxTexBinding tex;           // sampler + image description at draw time
  float scaleS = 1, scaleT = 1;   // image -> PICA texture coordinate scale (texfit.h); mirrored in the packet's texScale uniform
};

// Optional synchronous storage supplied by the consumer. A failed reservation
// must leave its allocator unchanged. The packet borrows these bytes until draw.
// (Claude v86) translate-once, resident geometry: the exact identity of a packed vertex / index buffer. Packing reads nothing else: the members' decoded
// geometry (retained ids: immutable), a group's slot maps, the layout (sig: kind, texcoord, register, components, type, offset per attribute), the stride
// and the planar choice; absent attributes get fixed constants.
struct ResidentGeometryKey {
  std::vector<uint64_t> ids; std::vector<int8_t> slots; std::vector<uint32_t> sig;
  uint32_t stride = 0, vertexCount = 0, indexCount = 0; bool planar = false;
  bool operator==(const ResidentGeometryKey& o) const {
    return stride == o.stride && vertexCount == o.vertexCount && indexCount == o.indexCount && planar == o.planar && ids == o.ids && slots == o.slots && sig == o.sig;
  }
};
struct ResidentGeometryHit { uint8_t* vertices = nullptr; uint16_t* indices = nullptr; bool missingNrm = false, missingClr = false, indicesChecked = false; };
struct ResidentGeometryLookup {
  const uint64_t* ids = nullptr; size_t idCount = 0;   // in: the retained geometry ids of the draw (a group: one per member, in order)
  void* context = nullptr;
  bool (*find)(void*, uint64_t hash, const ResidentGeometryKey&, ResidentGeometryHit&) = nullptr;
  ResidentGeometryKey key; uint64_t hash = 0; bool hit = false;   // out (fillPacket): the identity, its hash, whether the buffers came from find
};
struct PacketGeometryOutput {
  void* context = nullptr;
  bool (*allocate)(void*, size_t, size_t, uint8_t*&, uint16_t*&) = nullptr;
  ResidentGeometryLookup* resident = nullptr;   // (Claude v86) when set: buffers already resident in linear memory replace packing (externalVertices)
};

struct DrawPacket {
  bool ok = false;            // false: nothing to draw with this packet (see notes / pica.blockers)
  // The compiled draw. Packets built through a DrawCompileCache share it (const): read it with pica(); picaMut() gives a private copy to change (tests, tools).
  const PicaDraw& pica() const;
  std::weak_ptr<const PicaDraw> picaOwner() const { return pica_; } // cache identity without extending compiled-draw lifetime
  PicaDraw& picaMut();
  void setPica(std::shared_ptr<const PicaDraw> p) { pica_ = std::move(p); }
  std::shared_ptr<PackedUniforms> packedUniforms;
  bool usesPackedUniforms = false;
  UniformMap uniforms;        // keyed by the shader's uniform names (vtx.h host contract)
  std::vector<SubmitAttr> layout;
  uint32_t stride = 0;
  uint32_t vertexCount = 0;
  bool planar = false;               // (Claude v77) layout[i].offset is the start of attribute i's array (vertexCount elements, tightly packed); stride is their sum
  std::vector<uint8_t> vertices;     // interleaved, little endian
  std::vector<uint16_t> indices;     // triangle list
  uint8_t* externalVertices = nullptr;
  uint16_t* externalIndices = nullptr;
  size_t externalIndexCount = 0;
  const uint8_t* vertexData() const { return externalVertices ? externalVertices : vertices.data(); }
  size_t vertexBytes() const { return externalVertices ? size_t(stride)*vertexCount : vertices.size(); }
  const uint16_t* indexData() const { return externalIndices ? externalIndices : indices.data(); }
  size_t indexCount() const { return externalIndices ? externalIndexCount : indices.size(); }
  std::vector<HwStage> texenv;       // one per PICA texenv stage (hardware words, constants resolved)
  uint32_t texenvUpdateBuffer = 0;   // GPUREG_TEXENV_UPDATE_BUFFER value
  SubmitUnit units[3];
  GxViewport viewport;               // as decoded (EFB pixels)
  GxScissor scissor;
  uint8_t cullMode = 2;              // GXCullMode as decoded; frame.picaCull is the PICA setting
  PicaFrame frame;                   // GX viewport / scissor / cull / depth range mapped onto the target (frame.h); `uniforms["projMtx"]` already includes frame.post
  bool colorUpdate = true, alphaUpdate = true, dither = false;
  bool dstAlphaEnable = false;
  uint8_t dstAlpha = 0;
  uint8_t pixelFmt = 0;
  bool zCompBeforeTex = false;
  // log2 of (a target pixel / an EFB pixel) along the picture: the PICA measures the texture footprint per *target* pixel and the GX unit per EFB pixel, so every LOD the
  // PICA computes is this much too high (+1 at the usual half resolution). buildPlan adds it to the sampler's LOD bias (c3dplan.cpp).
  float lodFrameBias = 0;
  std::vector<std::string> notes;    // GxState::notes + pica.notes + packing notes
  // Retained geometry (PackedGeometryCache): vertices / indices are swapped in from packedFrom (give them back with releasePacked); packKey != 0: packed
  // here, may be kept with releasePacked. indicesChecked: every index is known to be below vertexCount (c3dplan skips its check).
  PackedGeometryCache::Entry* packedFrom = nullptr;
  uint64_t packKey = 0, packGeoId = 0;
  std::vector<uint32_t> packSig;
  bool packMissingNrm = false, packMissingClr = false;
  bool indicesChecked = false;
  // Two-pass draws (multipass.h): this packet is the first pass (pica().role == ScratchWrite) and `second` the packet of the second pass, built from pica().pass2 over the same geometry.
  std::shared_ptr<DrawPacket> second;
 private:
  std::shared_ptr<const PicaDraw> pica_;
};

// `cache`: when given, the compiled draw comes from (and goes into) the cache; Options::verifySamples should be 0 at run time (the verifier is a test tool).
DrawPacket buildDraw(const GxState&, const GxDraw&, const Options& = {}, const FrameConfig& = {}, DrawCompileCache* cache = nullptr);
// The same, into a packet the caller keeps from draw to draw: its vectors and uniform map keep their capacity (a run-time renderer makes no allocations
// per draw for them). `notes` false: the diagnostic notes of the state, the compiled draw and the frame mapping are not copied into P.notes (their
// strings are copies; a run-time renderer that does not show them saves the allocations). Every other field is as buildDraw returns it.
// packCache / geoId: the draw's geometry is retained (RetainedGeometry::id): its packed buffers may come from (and, through releasePacked, go to) packCache.
void buildDrawInto(DrawPacket& P, const GxState&, const GxDraw&, const Options& = {}, const FrameConfig& = {}, DrawCompileCache* cache = nullptr, bool notes = true,
                   PackedGeometryCache* packCache = nullptr, uint64_t geoId = 0, const PacketGeometryOutput* output = nullptr, const matrixbatch::Group* group = nullptr, std::shared_ptr<const PicaDraw> compiled = {}, std::shared_ptr<PackedUniforms> preparedUniforms = {});
// After the packet's buffers were consumed (copied to the GPU): gives swapped-in buffers back, or keeps freshly packed ones (planOk: c3dplan accepted them, so
// their indices were checked). The packet's vertex / index vectors may be left empty.
void releasePacked(DrawPacket& P, PackedGeometryCache& cache, bool planOk);

}  // namespace t2p
