// GX command stream (the GameCube GPU's hardware FIFO) -> register file + decoded draw calls.
//   The game's GX SDK writes exactly this byte stream (CP / XF / BP register loads, display-list calls, primitive draws), so this is the
//   hardware-exact seam between "the game" and "a renderer": GxFifo keeps the three register files up to date and, on every draw
//   command, decodes the vertex data (VCD/VAT driven, direct or indexed through host-supplied arrays) and assembles triangles.
//   Register fields are decoded on demand by gxstate.h. Game-agnostic. Big-endian stream.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include "gxdecodecache.h"
#include "decoded_vector.h"
#include "gxstateprogram.h"
#include "prepared_shape.h"
#include <array>

#ifdef __3DS__
extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
}
extern bool g_t2pProfOn;          // per-draw decode profiling (g_t2pProf); off by default: svcGetSystemTick is a system call (~1.3 us), and a busy frame has ~15,000 draws
// (Claude v94) sampled: with g_t2pProfOn, only every g_t2pProfPeriod-th GX draw is timed (g_t2pProfNow, set in handleDraw for that draw); the report
// multiplies by the period. Period 1 = every draw (the old behaviour).
extern uint32_t g_t2pProfPeriod;
extern thread_local bool g_t2pProfNow;
inline bool t2pProfEnabled() { return g_t2pProfNow; }
inline uint64_t t2pProfTicks() { return g_t2pProfNow ? svcGetSystemTick() : 0; }
#else
inline bool t2pProfEnabled() { return false; }
inline uint64_t t2pProfTicks() { return 0; }
#endif
extern thread_local uint64_t g_t2pProf[16];
extern thread_local uint64_t g_t2pStateProf[8];   // (Claude v100) gxstate.cpp section timers
// (Claude) N3DS_PROF_ORIGIN diagnostic: packets counted per stream origin (0 typed SDK calls, 1 raw display lists); [0] BP [1] CP [2] XF loads [3] XF words [4] indexed XF loads
extern bool g_gxOriginProf; extern int g_gxOrigin; extern uint64_t g_gxOriginCount[2][5];
extern unsigned long g_deferWhy[8];
extern uint32_t g_deferMin;
extern int g_benchRepeat[9];   // benchmark-only knobs (benchreplay3ds): [8] extra fast vertex attribute decodes per draw (Claude), [0] extra c3dFinalize calls per plan, [1] extra plan resets, [2] extra packUniformsPacked, [3] extra mapFrame, [4] extra texenv encode, [5] extra compile-cache get, [6] extra buildPlanInto, [7] extra TextureCache::get per plan unit   // profiling counters (gxfifo.cpp): [0] vertex decode ticks [1] sink ticks [2] vertices [3] state decode ticks [4] draw copies ticks [5] append [8] compile-cache get [9] vertex/index pack [10] rest of fillPacket [11] plan shader+attrs [12] plan uniforms [13] plan textures [14] plan rest [15] backend arena copy
namespace t2p {

// (Claude) Register groups of decodeState (gxstate.cpp), one bit each; a write that changes a register sets its group's bit in GxFifo.
// Registers decodeState does not read belong to no group. kStateDirtyOther: the vertex descriptor / formats (CP) and anything unclassified.
enum : uint32_t {
  kStateDirtyGen = 1u << 0,      // BP 0x00 (GEN_MODE)
  kStateDirtyTev = 1u << 1,      // BP 0x10..0x1F, 0x28..0x2F, 0xC0..0xDF, 0xF6..0xFD
  kStateDirtyTevReg = 1u << 2,   // BP 0xE0..0xE7 (both banks)
  kStateDirtyPixel = 1u << 3,    // BP 0x40..0x43, 0xF3
  kStateDirtyFog = 1u << 4,      // BP 0xEE..0xF2
  kStateDirtyView = 1u << 5,     // BP 0x20, 0x21, 0x59; XF 0x101A..0x101F
  kStateDirtyTex = 1u << 6,      // BP 0x80..0xBF, the Aurora texture addresses
  kStateDirtyXfMtx = 1u << 7,    // XF 0x000..0x5FF (position, texture, normal matrices)
  kStateDirtyLight = 1u << 8,    // XF 0x600..0x6FF
  kStateDirtyChan = 1u << 9,     // XF 0x100A, 0x100C, 0x100E, 0x1010
  kStateDirtyTexGen = 1u << 10,  // XF 0x1012, 0x1018, 0x1019, 0x1040..0x1057
  kStateDirtyProj = 1u << 11,    // XF 0x1020..0x1026
  kStateDirtyOther = 1u << 12,
  kStateDirtyVertexFormat = 1u << 13, // CP VCD/VAT: decoded geometry changes; state draw-dependent fields refresh unconditionally.
  kStateDirtyAll = 0xFFFFFFFFu,
};
extern bool g_stateCpIncremental; // experimental narrow VCD/VAT invalidation
inline uint32_t stateDirtyBp(uint32_t id) {
  if (id == 0x00) return kStateDirtyGen;
  if ((id >= 0x10 && id <= 0x1F) || (id >= 0x28 && id <= 0x2F) || (id >= 0xC0 && id <= 0xDF) || (id >= 0xF6 && id <= 0xFD)) return kStateDirtyTev;
  if (id >= 0xE0 && id <= 0xE7) return kStateDirtyTevReg;
  if ((id >= 0x40 && id <= 0x43) || id == 0xF3) return kStateDirtyPixel;
  if (id >= 0xEE && id <= 0xF2) return kStateDirtyFog;
  if (id == 0x20 || id == 0x21 || id == 0x59) return kStateDirtyView;
  if (id >= 0x80 && id <= 0xBF) return kStateDirtyTex;
  return 0;
}
inline uint32_t stateDirtyXf(uint32_t addr) {
  if (addr < 0x600) return kStateDirtyXfMtx;
  if (addr < 0x700) return kStateDirtyLight;
  if (addr == 0x100A || addr == 0x100C || addr == 0x100E || addr == 0x1010) return kStateDirtyChan;
  if (addr == 0x1012 || addr == 0x1018 || addr == 0x1019 || (addr >= 0x1040 && addr <= 0x1057)) return kStateDirtyTexGen;
  if (addr >= 0x101A && addr <= 0x101F) return kStateDirtyView;
  if (addr >= 0x1020 && addr <= 0x1026) return kStateDirtyProj;
  return 0;
}


// Internal cached vertex-format recipe. Array bindings and outputs refresh per draw.
struct GxAttrPlan {
  enum Kind : uint8_t { Byte, Pos, Nrm, Clr, Tex } kind;
  uint8_t type;            // GxAttrType: GXA_DIRECT / INDEX8 / INDEX16
  uint8_t fmt;             // component / colour format
  uint8_t nc;              // components read
  bool be;                 // the data is big endian
  int slot = 0;             // output texcoord/color/matrix-index slot
  int arrayId;
  uint32_t streamBytes;    // bytes this attribute takes in the stream
  uint32_t offset;         // byte offset of the attribute inside a vertex
  uint32_t need;           // bytes read per element
  float scale;             // 2^-shift for the integer formats (exact: a power of two)
  const uint8_t* base;     // registered array data (nullptr: guest memory through guestElement)
  uint32_t stride, size;
  uint8_t* out;            // first output element of this draw (float* / uint8_t* by kind)
  uint32_t outStride;      // output elements per vertex, in bytes
};


// ---- CP: vertex descriptor / attribute formats
enum GxAttrType : uint8_t { GXA_NONE = 0, GXA_DIRECT = 1, GXA_INDEX8 = 2, GXA_INDEX16 = 3 };
enum GxCompFmt : uint8_t { GXC_U8 = 0, GXC_S8 = 1, GXC_U16 = 2, GXC_S16 = 3, GXC_F32 = 4 };
enum GxClrFmt : uint8_t { GXCL_RGB565 = 0, GXCL_RGB8 = 1, GXCL_RGBX8 = 2, GXCL_RGBA4 = 3, GXCL_RGBA6 = 4, GXCL_RGBA8 = 5 };
enum GxPrim : uint8_t { GXP_QUADS = 0x80, GXP_QUADS2 = 0x88, GXP_TRIANGLES = 0x90, GXP_TRISTRIP = 0x98, GXP_TRIFAN = 0xA0, GXP_LINES = 0xA8, GXP_LINESTRIP = 0xB0, GXP_POINTS = 0xB8 };

// array ids (CP array registers 0xA0 + id / 0xB0 + id)
enum GxArrayId : uint8_t { GXAR_POS = 0, GXAR_NRM = 1, GXAR_CLR0 = 2, GXAR_CLR1 = 3, GXAR_TEX0 = 4, /* .. TEX7 = 11 */ GXAR_POSMTX = 12, GXAR_NRMMTX = 13, GXAR_TEXMTX = 14, GXAR_LIGHT = 15, GXAR_COUNT = 16 };

struct GxVcd {                    // CP 0x50 / 0x60
  uint8_t pmIdx = 0;              // per-vertex position matrix index byte present
  uint8_t texMtxIdx[8] = {};      // per-vertex texture matrix index bytes present
  uint8_t pos = 0, nrm = 0, clr[2] = {}, tex[8] = {};   // GxAttrType
};
struct GxVat {                    // CP 0x70+n / 0x80+n / 0x90+n
  uint8_t posCnt = 1, posFmt = 4, posShift = 0;         // cnt: 0 = XY, 1 = XYZ
  uint8_t nrmCnt = 0, nrmFmt = 4; bool nrmIdx3 = false; // cnt: 0 = XYZ, 1 = NBT
  uint8_t clrCnt[2] = {1, 1}, clrFmt[2] = {5, 5};       // cnt: 0 = RGB, 1 = RGBA
  uint8_t texCnt[8] = {1, 1, 1, 1, 1, 1, 1, 1}, texFmt[8] = {4, 4, 4, 4, 4, 4, 4, 4}, texShift[8] = {};   // cnt: 0 = S, 1 = ST
};

// Register files. bp[] holds the last value written (24 bits of data + the register id in the top byte, as in the stream).
struct GxRegs {
  uint32_t bp[256] = {};
  bool bpWritten[256] = {};
  uint32_t cp[256] = {};
  uint32_t xf[0x1100] = {};       // XF memory: 0x0000.. matrices, 0x0400.. normal matrices, 0x0500.. dual-tex matrices, 0x0600.. lights, 0x1000.. registers
  // BP 0xE0..0xE7 hold two register banks behind the same ids (bit 23 selects): TEV colour registers PREV, REG0..2 (S11 per component)
  // and the konstant colours KCOLOR0..3 (8 bit). tevRaw[bank][id - 0xE0] is the last 24-bit value written to each; bank 0 = TEV, 1 = konst.
  uint32_t tevRaw[2][8] = {};
  // Aurora streams (GxFifo::setAuroraMode): full 32-bit addresses that the hardware's 26-bit address fields cannot hold. 0 = use the BP register field.
  uint32_t texAddr[8] = {};
  bool auroraViewport = false;   // Aurora encodes XF viewport origin with340; scissor still uses342
  uint32_t copyDstAddr = 0;
  uint32_t tlutSrcAddr = 0;
  GxVcd vcd() const;
  GxVat vat(int idx) const;
  float xfFloat(uint32_t addr) const;
};

// A host-owned vertex array the CP indexes into (GXSetArray).
struct GxArray {
  const uint8_t* data = nullptr;
  uint32_t size = 0;              // bytes
  uint32_t stride = 0;
  bool bigEndian = true;          // GC data files are big endian; set false for arrays the host already converted
  bool strideFromCp = false;      // stride = CP 0xB0 + id (Aurora's GX_AURORA_LOAD_ARRAYBASE leaves the stride in the CP register)
};

// Decoded vertices of one draw command, de-indexed, full precision; only attributes present in the VCD are filled.
struct GxVertices {
  uint32_t count = 0;
  bool hasPos = false, hasNrm = false, hasClr[2] = {}, hasTex[8] = {}, hasPmIdx = false, hasTexMtxIdx[8] = {};
  DecodedVector<float> pos;         // 3 per vertex (z = 0 for XY)
  DecodedVector<float> nrm;         // 3 per vertex (the normal of an NBT triple)
  DecodedVector<uint8_t> clr[2];    // 4 per vertex (RGBA8, alpha 255 for RGB formats)
  DecodedVector<float> tex[8];      // 2 per vertex (t = 0 for S only)
  DecodedVector<uint8_t> pmIdx;     // position matrix row index (GX: 3 * PNMTX number)
  DecodedVector<uint8_t> texMtxIdx[8];
};

struct GxDraw {
  uint8_t prim = 0;               // GxPrim
  uint8_t vatIdx = 0;
  uint32_t commandCount = 1;      // original GX primitive commands represented by this draw
  uint32_t vertexCount = 0;       // vertices as sent
  GxVertices v;
  DecodedVector<uint32_t> tris;     // triangle list (3 indices per triangle) into v; empty for lines / points
  bool unsupportedPrim = false;   // lines / points: not assembled
  // Retained geometry (gxretain.h): a sink's batch target whose draws were not decoded. v.count / vertexCount / commandCount advance as usual, but the vertex
  // vectors and tris stay empty; retainedTris is the triangle index count decoding would have produced. The geometry is held elsewhere (RetainedGeometry).
  bool retained = false;
  uint32_t retainedTris = 0;
  GxVcd vcd;
  GxVat vat;
};

// The inputs of one GX draw command (or one primitive run), as decoding sees them: everything the decoded vertices and triangles are a function of,
// apart from the bytes of the indexed arrays (attrs[k].base / stride / size). Valid during the observer call only.
struct GxDrawInputs {
  uint8_t cmd = 0;                       // the (first) draw opcode: prim | vatIdx
  int partCount = 0;                     // primitive run parts (0: a single command)
  const uint8_t* partPrim = nullptr;     // per part: prim (stride partStride bytes)
  const uint32_t* partCount32 = nullptr; // per part: vertex count
  size_t partStride = 0;                 // bytes between consecutive parts' entries
  uint32_t vertices = 0, vsize = 0;
  const uint8_t* stream = nullptr;       // vertices * vsize bytes
  const uint32_t* fmtRaw = nullptr;      // 5 words: VCD lo/hi, VAT A/B/C of vatIdx
  int nAttrs = 0;
  const GxAttrPlan* attrs = nullptr;
  bool fast = false;                     // every indexed attribute has a registered array (base set)
  // (Claude v95) shape references (GX_AURORA 0x7D): the draw is primitive #shapeDraw of an immutable J3D shape display list whose identity, vertex
  // format and array bindings hash to shapeKey (0: not in a shape reference). A retainer may then match it on (shapeKey, shapeDraw) alone.
  uint64_t shapeKey = 0;
  uint32_t shapeDraw = 0;
};

// triangle index count of a (possibly retained, see GxDraw::retained) draw
inline size_t gxTriCount(const GxDraw& d) { return d.retained ? d.retainedTris : d.tris.size(); }

// (Claude v80) an ordered UI route marker (GX_AURORA debug marker "\x01WWROUTE1 ..."; n3ds ui_route_3ds.h): begin = the draws up to the matching end
// go to the bottom screen; src = the region in the draws' 2D projection input space, dst = bottom-screen pixels (320x240, origin top-left).
struct GxUiRoute { bool begin = false; int16_t src[4] = {}; int16_t dst[4] = {}; };   // left, top, width, height

class GxSink {
 public:
  virtual ~GxSink() {}
  virtual void onDraw(const GxRegs&, const GxDraw&) = 0;
  // Optional decode-in-place path (saves copying every vertex into a batch): before decoding, the sink sees the draw's header (prim, vatIdx, vertexCount,
  // vcd, vat, unsupportedPrim; no vertices) and may name the draw the vertices are to be appended to: either an earlier draw (same vcd: canAppendHeader)
  // or a new one it has set up with this header and empty vertices. The fifo then appends the vertices and triangles (indices offset by the earlier
  // vertex count; a non-empty target becomes a triangle list) and calls onAppended instead of onDraw. nullptr: decode into the fifo's own draw, onDraw.
  virtual GxDraw* appendTarget(const GxRegs&, const GxDraw& header) { (void)header; return nullptr; }
  virtual void onAppended(const GxRegs&, GxDraw& into, uint32_t firstVertex) { (void)into; (void)firstVertex; }
  // Retained geometry (gxretain.h; only with GxFifo::setRetainOffers(true) and an appendTarget). Before decoding a draw into its target the fifo offers its
  // inputs: true skips decoding (the sink sets D.retained; the fifo still advances D's counts and calls onAppended). retainDecoded follows a draw that was
  // offered and decoded (vertices from firstVertex, triangle indices from firstTri).
  virtual bool retainOffer(const GxDrawInputs&, GxDraw& D) { (void)D; return false; }
  virtual void retainDecoded(const GxDrawInputs&, const GxDraw& D, uint32_t firstVertex, size_t firstTri, bool arrayError) { (void)D; (void)firstVertex; (void)firstTri; (void)arrayError; }
  // Opt in only for sinks that merge consecutive compatible draws. Return
  // remaining room in the current batch so original batch boundaries survive.
  virtual uint32_t primitiveRunLimit() const { return 0; }
  virtual void onDrawDone(const GxRegs&) {}
  virtual void onCopy(const GxRegs&) {}        // BP 0x52 written: EFB copy to a texture / the XFB (decode with gxstate.h)
  virtual void onTlutLoad(const GxRegs&) {}    // BP 0x65 written: palette load into TMEM
  virtual void onUiRoute(const GxRegs&, const GxUiRoute&) {}   // (Claude v80) see GxUiRoute; the default ignores it (the draws stay where they were)
  virtual void onError(const std::string&) {}
  // CALL_DL commands carry a (physical) address; the host maps it to display list bytes (nullptr = cannot)
  virtual const uint8_t* resolveDisplayList(uint32_t addr, uint32_t size) { (void)addr; (void)size; return nullptr; }
};

class GxFifo {
 public:
  explicit GxFifo(GxSink* sink = nullptr) : sink_(sink) {}
  void setSink(GxSink* s) { sink_ = s; }
  void setArray(int id, const GxArray& a) { arrays_[id] = a; }
  GxArray& array(int id) { return arrays_[id]; }
  // Arrays the host did not register are read from guest memory the way the CP does: base = CP 0xA0 + id (physical address), stride = CP 0xB0 + id.
  // Used to replay recorded command streams (Dolphin FIFO logs), whose vertex data lives in the recorded guest memory. nullptr from the reader = unmapped.
  using GuestRead = std::function<const uint8_t*(uint32_t addr, uint32_t size)>;
  void setGuestArrayReader(GuestRead f) { guestRead_ = std::move(f); }
  const GxRegs& regs() const { return regs_; }
  GxRegs& regs() { return regs_; }
  // Processes a complete command stream (display lists are complete streams). Returns the number of bytes consumed; stops early on an
  // unknown opcode or a truncated command (reported through onError).
  size_t process(const uint8_t* data, size_t size);
  uint64_t drawCount() const { return draws_; }
  // Bumped whenever a register that decodeState reads changes value (CP array base / stride are not counted: they only locate vertex data, which each GxDraw carries decoded).
  // Two draws that see the same generation were drawn with identical register state, so a sink may merge them (gxbatch.h).
  uint64_t stateGeneration() const { return stateGen_; }
  void setStaticTracking(bool on){staticTracking_=on;staticRevision_=1;}
  uint32_t staticRevision()const{return staticTracking_&&staticRevision_!=UINT32_MAX?staticRevision_:0;}

  // (Claude) Which groups of registers that decodeState reads changed value since the last takeStateDirty() (gxstate.h kStateDirty*): a sink
  // that decodes one state after another passes it with the previous state to decodeStateInto, which then redoes only those sections.
  uint32_t takeStateDirty() { const uint32_t d = stateDirty_; stateDirty_ = 0; return d; }
  void setPrimitiveRuns(bool on) { primitiveRuns_ = on; }
  void setGeometryCache(bool on) { if(on&&!decodeCache_)decodeCache_=std::make_unique<GxDecodeCache>();else if(!on)decodeCache_.reset(); }
  GxDecodeCacheStats geometryCacheStats() const { return decodeCache_ ? decodeCache_->stats() : GxDecodeCacheStats{}; }
  void setFastAttributeDecode(bool on) { fastAttributeDecode_ = on; }
  // (Claude) Deferred vertex decode: with a helper thread calling runDecodeJob(), a draw's float / colour attributes are decoded off the
  // processing thread (the matrix-index bytes, the only vertex data a sink reads in onAppended, stay inline). Same decoded values. The
  // sink must not read the other vertex attributes before finishDecodes() returned (PE_DONE calls it; a sink that hands frames on calls
  // it too). The index stream is copied; the vertex arrays must stay unchanged until then (as they already must for a FIFO worker thread).
  // `wake` is called on the processing thread after a job was queued.
  void setDeferredDecode(bool on, void (*wake)() = nullptr);
  bool deferredDecodeActive() const { return bool(deferQ_); }
  void setDeferredDecodePaused(bool on) { deferPaused_ = on; }   // (Claude v81) processing thread, between frames: decode inline while paused (no new jobs)
  bool runDecodeJob();      // any thread: runs one queued job; false: none waiting
  void finishDecodes();     // processing thread: returns when every queued job is done (runs waiting ones itself)
  uint64_t deferredJobs() const { return deferredJobs_; }
  uint64_t deferredSelfRun() const { return deferredSelf_; }   // jobs the processing thread ran itself in finishDecodes
  uint64_t deferredDrains() const { return deferredDrains_; }   // finishDecodes calls that found work pending
  void setCachedVertexPlan(bool on) { cachedVertexPlan_ = on; }
  // Aurora mode: the stream comes from Aurora's GX SDK (the native port's GX layer) instead of the console's. It adds the GX_AURORA (0x50) command, whose
  // subcommands carry host pointers (vertex arrays, texture / palette objects, copy destinations) and draws of a byte length; the pointers are resolved through
  // the guest array reader (on the 3DS: identity). Palette loads and texture copies are completed from those subcommands (onTlutLoad fires on GX_AURORA_LOAD_TLUT).
  void setAuroraMode(bool on) { if(aurora_!=on)touchStatic();aurora_ = on; regs_.auroraViewport = on; stateDirty_ = kStateDirtyAll; }
  bool auroraMode() const { return aurora_; }
  // process() returns right after a draw-done command (BP 0x45) when on; drawDoneHit() then tells it stopped there (Aurora's command processor contract).
  void setStopAtDrawDone(bool on) { stopAtDrawDone_ = on; }
  // Live game (host pointers, identity reader): GX_AURORA array sizes are not trusted for bounds checks (the game declares placeholders).
  void setUncheckedArrays(bool on) { uncheckedArrays_ = on; }
  bool takeDrawDone() { const bool h = drawDoneHit_; drawDoneHit_ = false; return h; }
  // (Claude) Typed-GX bypass entry points (item 3): each does exactly what process() does for one 0x61 BP /
  // 0x08 CP / 0x10 XF packet (BP mask, PE_DONE draw-done and the state generation included). Stream order is
  // the caller's job: every byte written to the FIFO before the call must already have been processed.
  // They do not count as a process() call (processSerial()).
  bool processStateProgram(const uint8_t* data, size_t size);
  bool processShapeProgram(uint64_t hint,const uint8_t* data,size_t size);
  // (Claude v95) GX_AURORA 0x7D: a J3D shape display list by reference (not copied into the stream). The producer sends it only for lists whose
  // vertex arrays are the model's own immutable data; `gen` changes whenever model memory may have been freed. Processed exactly like the list
  // itself; its draws carry a shape key (GxDrawInputs::shapeKey). Returns false when the pointer cannot be resolved.
  uint64_t materialRefs_=0;
  uint64_t materialRefs()const{return materialRefs_;}
  uint64_t modelGroups_=0;
  bool loadPreparedShapes(const char* path){try{auto store=std::make_unique<PreparedShapeStore>();if(!store->open(path))return false;preparedShapes_=std::move(store);return true;}catch(...){return false;}}
  uint64_t preparedShapeHits()const{return preparedShapes_?preparedShapes_->hits:0;}
  uint64_t preparedShapeMisses()const{return preparedShapes_?preparedShapes_->misses:0;}
  uint64_t preparedShapeRuns()const{return preparedShapeRuns_;}
  size_t preparedShapeBytes()const{return preparedShapes_?preparedShapes_->bytes():0;}
  bool processShapeRef(const uint8_t* dl, uint32_t size, uint32_t gen);
  uint64_t shapeRefs() const { return shapeRefs_; }
  uint64_t modelGroups()const{return modelGroups_;}
  void setShapeRefs(bool on) { shapeRefsOn_ = on; }
  uint64_t shapeProgramHits()const{return shapeProgramHits_;}
  uint64_t shapeProgramMisses()const{return shapeProgramMisses_;}
  uint64_t stateProgramHits() const { return stateProgramHits_; }
  uint64_t stateProgramMisses() const { return stateProgramMisses_; }
  void applyProgramBp(uint32_t v,uint32_t dirty) { handleBpPrepared(v,dirty); }
  void applyProgramXf(uint32_t addr,const uint32_t* words,uint32_t n);
  void applyBp(uint32_t v) {
    handleBp(v);
    if ((v >> 24) == 0x45) { finishDecodes(); if (sink_) sink_->onDrawDone(regs_); if (stopAtDrawDone_) drawDoneHit_ = true; }
  }
  void applyCp(uint8_t addr, uint32_t v) { handleCp(addr, v); }
  void applyXf(uint32_t addr, const uint8_t* bigEndianWords, uint32_t n) { handleXf(addr, bigEndianWords, n); }
  // Analysis / retention: called for every draw before its vertices are decoded (nullptr: off).
  void setInputObserver(std::function<void(const GxDrawInputs&)> f) { inputObserver_ = std::move(f); }
  void setRetainOffers(bool on) { retainOffers_ = on; }
  bool retainOffers() const { return retainOffers_; }
  // Serial number of the current top-level process() call (memory the stream reads cannot change during one call).
  uint64_t processSerial() const { return processSerial_; }

 private:
  void handleBp(uint32_t v);
  void handleBpPrepared(uint32_t v,uint32_t dirty);
  void handleCp(uint8_t addr, uint32_t v);
  void handleXf(uint32_t addr, const uint8_t* words, uint32_t n);
  void indexedXf(int arrayId, uint16_t index, uint16_t addrLen);
  using PrimitiveRunPart=GxPrimitiveRunPart;
  int tryDrawRun(const uint8_t*& p,const uint8_t* end);
  bool handleDraw(uint8_t cmd, const uint8_t*& p, const uint8_t* end, int count = -1, const PrimitiveRunPart* parts = nullptr, int partCount = 0);
  std::unique_ptr<PreparedShapeStore> preparedShapes_;
  uint64_t preparedShapeRuns_=0;
  bool executePreparedShape(const PreparedShape&);
  bool handleAurora(const uint8_t*& p, const uint8_t* end);
  bool aurora_ = false;
  bool stopAtDrawDone_ = false, drawDoneHit_ = false, uncheckedArrays_ = false;
  uint8_t auroraTlutFmt_[32] = {};    // palette format of each TLUT slot (GX_AURORA_LOAD_TLUT)
  uint32_t auroraCopyBits_ = 0;       // BP 0x52 fields GXSetTexCopyDst would have set (format, mipmap, intensity)
  bool auroraCopyZ_ = false;          // the copy format is a Z format
  void err(const std::string& s) { if (sink_) sink_->onError(s); }

  std::array<std::unique_ptr<GxStateProgram>,128> statePrograms_;
  std::array<uint64_t,128> stateProgramAges_{};
  uint64_t stateProgramClock_=0;
  size_t stateProgramBytes_=0;
  uint64_t stateProgramHits_=0, stateProgramMisses_=0;
  struct ShapeProgram {
    struct Part {uint32_t offset,count,end;uint8_t cmd;};
    struct Zero {uint32_t offset,size;};
    uint64_t hint=0;size_t size=0;uint32_t format[5]={};uint8_t vat=0;
    std::vector<Part> parts;std::vector<Zero> zeros;
    size_t cost()const{return sizeof(*this)+parts.capacity()*sizeof(Part)+zeros.capacity()*sizeof(Zero);}
  };
  std::array<std::unique_ptr<ShapeProgram>,64> shapePrograms_;
  size_t shapeProgramBytes_=0;
  uint64_t shapeProgramHits_=0,shapeProgramMisses_=0;
  uint64_t shapeKey_=0,shapeRefs_=0; uint32_t shapeDraw_=0; bool shapeRefsOn_=true;   // (Claude v95)
  GxSink* sink_ = nullptr;
  std::function<void(const GxDrawInputs&)> inputObserver_;
  bool retainOffers_ = false;
  uint64_t processSerial_ = 0;
  int processDepth_ = 0;
  GxRegs regs_;
  GxArray arrays_[GXAR_COUNT];
  GuestRead guestRead_;
  const uint8_t* guestElement(int arrayId, uint32_t index, uint32_t need) const;
  uint64_t draws_ = 0;
  uint32_t bpMask_ = 0xFFFFFF;    // BP register 0xFE: bits the next BP write is allowed to change
  uint64_t stateGen_ = 0;
  GxDraw scratch_;                // handleDraw decodes into this (reused, see there)
  // The vertex format, decoded once per change of its raw registers (CP 0x50 / 0x60 and the three VAT words of each format) instead of on every draw
  struct FmtCache { bool valid = false; uint32_t raw[5] = {}; GxVcd vcd; GxVat vat; uint32_t vsize = 0; bool planned = false; int nAttrs = 0; GxAttrPlan attrs[21]; };
  bool primitiveRuns_ = false;
  std::array<PrimitiveRunPart,128> runParts_;
  std::vector<uint8_t> runBytes_;
  bool splitRun_ = false;
  void uiRouteMarker(const char* text, uint32_t n);   // (Claude v80) a "\x01WWROUTE1" debug marker: GxSink::onUiRoute, batch boundary   // (Claude v79) handleDraw of a primitive run reads each part where it lies (runParts_; no copy into runBytes_)
  std::unique_ptr<GxDecodeCache> decodeCache_;
  bool fastAttributeDecode_ = true;
  struct DecodeQueue;
  struct DecodeQueueDelete { void operator()(DecodeQueue*) const; };
  std::unique_ptr<DecodeQueue, DecodeQueueDelete> deferQ_;
  bool deferPaused_ = false;
  uint64_t deferredJobs_ = 0, deferredSelf_ = 0, deferredDrains_ = 0;
  bool cachedVertexPlan_ = true;
  FmtCache fmt_[8];
  FmtCache& vertexFormat(int vatIdx);
  bool staticTracking_=false;uint32_t staticRevision_=1;
  void touchStatic(){if(staticTracking_&&staticRevision_!=UINT32_MAX)++staticRevision_;}
  void setReg(uint32_t& r, uint32_t v, uint32_t group) {
    if(r!=v){
      constexpr uint32_t groups=kStateDirtyGen|kStateDirtyTev|kStateDirtyTevReg|kStateDirtyPixel|kStateDirtyFog|kStateDirtyView|kStateDirtyTex|kStateDirtyChan|kStateDirtyTexGen|kStateDirtyProj;
      if((group&groups)&&staticTracking_){
        const bool colour=&r==&regs_.xf[0x100a]||&r==&regs_.xf[0x100c];
        const bool selected=&r==&regs_.xf[0x1018]&&((r^v)&~63u)==0;
        if(!colour&&!selected)touchStatic();
      }
      r=v;stateGen_++;stateDirty_|=group;
    }
  }
  uint32_t stateDirty_ = kStateDirtyAll;
};

// size in bytes of one vertex with direct data / indices as the VCD+VAT describe (0 when nothing is sent)
uint32_t gxVertexSize(const GxVcd&, const GxVat&);
// bytes of a texture image as GXGetTexBufferSize computes them (all levels down to 1x1 with `mips`)
uint32_t gxTexBufferSize(uint32_t w, uint32_t h, uint32_t fmt, bool mips);

}  // namespace t2p
