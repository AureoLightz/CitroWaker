#include "uniform_recipe.h"
// The frame loop of the port: GX command bytes in, a picture on the 3DS top screen out.
//
//   guest GX FIFO bytes --process()--> GxFifo --> events (draws, EFB copies, palette loads) recorded per frame
//   renderFrame():  C3D_FrameBegin -> clear of the previous frame's GXCopyDisp -> every event in order (buildDraw -> buildPlan -> Backend3ds::draw, planEfbCopy -> copy) -> C3D_FrameEnd
//
// Why events are recorded and replayed instead of drawn as they arrive: the frame policy for fog needs every draw of the frame before the first one can be compiled (one W-buffer
// range for all draws when anything fogs, draw.h frameFogRange), and a frame is only finished by the game's GXCopyDisp (the XFB copy). The recording holds the decoded GX state of each
// draw (GxState, a few KB) and the vertices; palette loads keep a copy of their entries (the game reloads TMEM between draws). Texture images are read from guest memory when the
// frame is replayed: a texture the game rewrites in the middle of a frame shows its last contents for the whole frame (GX would use the contents at the time of the draw).
//
// The XFB copy: the port presents the main target itself (C3D_FrameEnd transfers it to the LCD). GXCopyDisp with the clear flag clears the EFB for the next frame, but the picture must
// leave first, so the clear is planned and drawn at the *start of the next frame* with that frame's depth map; without the flag the EFB simply stays as it is. The very first frame starts
// with a black, far-depth target.
//
// Threads: not thread safe. The game side calls process(); the render side calls renderFrame(). Use one thread, or put a lock around both (the port decides). framesReady() tells how many
// finished frames are queued: a game running ahead of the GPU should be held back when it is 2 or more (the VI retrace wait does that on the real console).
#pragma once
#include <cstdio>
#include "texpack.h"
#include "texture_store.h"
#include "drawreuse3ds.h"
#include <unordered_map>
#include <unordered_set>
#include <3ds.h>
#include <citro3d.h>

#include <deque>
#include "state_vector_pool3ds.h"
#include <functional>
#include <memory>
#include <atomic>
#include <string>
#include <vector>

#include "backend3ds.h"
#include "bundle.h"
#include "c3dplan.h"
#include "drawcache.h"
#include "efbcopy.h"
#include "frame.h"
#include "gxbatch.h"
#include "matrixbatch.h"
#include "gxfifo.h"
#include "direct_draw_binding.h"
#include "gxretain.h"
#include "gxstate.h"
#include "submit.h"
#include "packet_worker3ds.h"
#include "texcache.h"
#include "translate.h"

namespace t2p {

struct RendererOptions {
  FrameConfig frame;                      // EFB size, target size, fit; init() calls resolve()
  Options translate = [] { Options o; o.verifySamples = 0; o.approxEffort = 0; return o; }();   // translator options (quality knobs); frameFogRange is set per frame. verifySamples defaults to 0 here: the verifier is a test tool; approxEffort 0: a material that cannot be drawn exactly gets the one-compile last resort at run time (the full degradation search takes seconds: precompile offline)
  bool materialCacheFastPath = true;      // exact MRU comparison before full material hashing
  size_t drawCacheEntries = 4096;         // compiled draws kept (DrawCompileCache); 0: no cache
  TexCacheOptions texCache = [] { TexCacheOptions t; t.tryEtc1 = false; t.sampleOncePerFrame = true; return t; }();   // no ETC1 at run time (a second per texture on the ARM11; RGB565 / RGBA4 / exact instead)
  size_t arenaBytes = 6u << 20;           // Backend3ds arena
  bool packetWorker=false,packetWorkerVerify=false;
  bool frameTiming = false; // coarse sampled waits, no per-draw calls
  bool planTemplates = false; // bounded exact templates, experimental
  bool matrixBatchProfile = false; // sampled diagnostic only
  bool matrixBatch = false; // exact adjacent pose-palette grouping, experimental
  bool directPack = false; // same interleaved bytes, written into bounded GPU arena
  bool planNotes = true; // optional text only; render state and failure reasons are unchanged
  bool packetNotes = true;                // DrawPacket::notes filled (DrawReport::packetNotes); false saves their string copies on every draw
  uint32_t batchMaxVertices = kBatchMaxVertices;   // consecutive draws with identical register state are merged up to this many vertices (gxbatch.h); 0: no batching
  uint8_t initialClear[4] = {0, 0, 0, 255};   // r g b a of the target before the first GXCopyDisp
  uint8_t frameFlags = 0;                 // C3D_FrameBegin flags (C3D_FRAME_SYNCDRAW: wait for the GPU at the end of the frame)
};

struct DrawReport {
  int index = 0;                          // draw number within the frame
  bool packetOk = false, planOk = false, drawn = false;
  bool twoPass = false;                   // drawn as two passes through the scratch target (multipass.h)
  uint32_t vertices = 0, triangles = 0;
  std::string why;                        // empty when drawn
  const std::vector<std::string>* packetNotes = nullptr;   // valid during the callback only
  const std::vector<std::string>* planNotes = nullptr;
  double buildMs = 0, planMs = 0, drawMs = 0;
};

struct CopyReport {
  int index = 0;                          // copy number within the frame (texture copies and the clear of the previous XFB copy)
  bool toXfbClear = false;                // the clear a GXCopyDisp asked for, drawn at the start of this frame
  const GxCopy* copy = nullptr;
  const EfbCopyPlan* plan = nullptr;
  bool done = false;
  std::string why;
  double planMs = 0, drawMs = 0;
};

extern uint64_t g_replayWhy[4];   // (Claude v117 diag) draw replay misses by reason
struct FrameReport {
  uint32_t packetJobs=0,packetChecks=0,packetMismatches=0;
  uint64_t packetWaitTicks=0;
  bool rendered = false;                  // false: nothing was ready
  bool skipped = false;                   // setSkipNextFrame honoured: the frame was consumed without presenting it (see there)
  uint32_t draws = 0, drawn = 0, refused = 0, copies = 0, copiesDone = 0, tlutLoads = 0;
  uint32_t gxDraws = 0;                   // GX draw commands the frame's draws were merged from (gxbatch.h)
  uint32_t clipTests=0,clipCulled=0;
  uint32_t twoPass = 0;                   // draws that went through the scratch target
  uint64_t beginWaitTicks=0,endSubmitTicks=0;
  uint64_t matrixSplit[6]={};   // (Claude v78 diag) frameTiming: key+lookup, appends, variant+uniforms, calibration pair, groups formed, members
  uint64_t splitTicks[8]={};   // (Claude) with frameTiming: frame policy, cache.beginFrame, whole event loop, EFB copies, frame end + recycle, texture conversions
  C3dPlanTemplateStats templateStats;
  uint32_t reuseHits=0,reuseMisses=0,reuseInserts=0,reuseEvicted=0,reuseFull=0;size_t reuseBytes=0,reuseEntries=0;   // (Claude v73) draw reuse
  uint32_t planDeltas=0,planDeltaFails=0,planDeltaChecks=0,planDeltaMismatches=0;   // (Claude v87) plan delta
  uint32_t residentHits=0,residentMisses=0,residentInserts=0,residentFull=0;size_t residentBytes=0,residentEntries=0;   // (Claude v86) resident geometry
  uint32_t modelProgramHits=0,modelProgramDraws=0,modelProgramReject[6]={};
  uint32_t modelGroupPrograms=0,modelGroupMembers=0;
  uint32_t uniformDeltaHits=0;
  uint32_t replayHits=0,replayMisses=0,replayInserts=0,replayStale=0,replayEntries=0,replayProj=0,replayKb=0,replayRecorded=0;
  uint32_t directDraws=0,directFallbacks=0,directGroups=0;
  uint32_t replayGroupHits=0,replayGroupMisses=0;   // (Claude v122) matrix groups (counted in replayHits too)   // (Claude v117) draw replay
  uint32_t routeMarkers=0,routedDraws=0,routeRefused=0,routeCopies=0,routeSwitches=0;   // (Claude v80) UI routing: markers, GX draws drawn on the bottom target, routed draws refused (two-pass / degenerate), copies inside a routed span (done on the top target), target switches
  uint64_t matrixTicks[3]={};uint32_t matrixSamples=0,matrixAttempts=0;
  float fogRange = 0;                     // W-buffer range of the frame (0: z map)
  double buildMs = 0, planMs = 0, drawMs = 0;
};

struct RecordedEvent3ds {
  enum Kind { Draw, TexCopy, Xfb, Tlut, Route, DirectDraw } kind = Draw;   // (Claude v80) Route: a UI route marker (route)
  DeferredDrawState stateStorage;
  RecordedEvent3ds(){} // user-provided: do not zero the unused legacy-state union
  GxState& state(){return stateStorage.state();}
  const GxState& state()const{return stateStorage.state();}
  const GxState& viewState()const{return stateStorage.view();}
  bool isDraw()const{return kind==Draw||kind==DirectDraw;}
  void setDirect(std::shared_ptr<const DirectDrawBinding> owner,uint32_t row,uint32_t count){stateStorage.setDirect(std::move(owner),row,count);kind=DirectDraw;}
  void legacy(){if(kind==DirectDraw){stateStorage.reset();kind=Draw;}}
  std::shared_ptr<const DrawRegisterSignature> rawSignature; // cold binding only

  GxDraw d{};                           // Draw (possibly several GX draws merged, gxbatch.h); header only when geo is set
  std::shared_ptr<const RetainedGeometry> geo;   // Draw: the batch's decoded geometry is shared, retained geometry (gxretain.h); null: it is in d
  uint32_t merged = 1;                  // Draw: GX draw commands in d
  uint32_t dirtyIn = 0xFFFFFFFFu;       // (Claude v78) Draw: GX state groups (kStateDirty*) whose registers changed since the previous Draw event's state, when
                                        // this state was decoded incrementally from it (record_frame_3ds.h); all bits: unknown
  uint64_t rk[2] = {0, 0};              // (Claude v121) Draw with geo: replay key (computeReplayKey, FIFO worker after recording); 0,0 = not computed
  int maxPm = -1;                       // Draw: maxPosMtx(d), kept as draws are appended (rescanning the batch made batching quadratic)
  GxCopy c{};                             // TexCopy, Xfb
  GxTlutLoad tl{};                        // Tlut
  std::vector<uint8_t> tlutBytes;       // Tlut: the entries as they were when the game loaded them
  GxUiRoute route{};                      // Route
};

// (Claude v86) Recorded events are larger than 512 bytes, so std::deque gives each its own heap node: a locked newlib malloc per event on the producer and
// a locked free per event on the renderer (~1100 a frame in the outset fixture). Their nodes come from a pool instead (sized for one event; the deque's
// pointer map is a different size and goes to operator new). trim() under heap pressure (gpu_3ds.cpp).
void computeReplayKey(RecordedEvent3ds& e);   // (Claude v121) renderer3ds.cpp: fills e.rk (Draw events with retained geometry)
struct EventNodePool { static FixedBlockPool& get() { static FixedBlockPool* p = new FixedBlockPool(768, sizeof(RecordedEvent3ds)); return *p; } };
using EventDeque3ds = std::deque<RecordedEvent3ds, PoolAlloc<RecordedEvent3ds, EventNodePool>>;
// Independent producer-owned storage. Only transfer after recording completes;
// swap it back after replay before the producer reuses its pool.
struct RecordedFrame3ds {
  std::vector<PVec> directRows;
  uint32_t directCaptured=0,directRejected=0,directChecks=0,directMismatches=0;
  uint32_t directDiag[16]={};
  EventDeque3ds events;
  StateVectorPool3ds statePool;
  std::vector<GxDraw> drawPool;
  size_t drawPoolRead=0;
  size_t drawPoolBytes = 0, framesReady = 0;
};

class Renderer3ds : public GxSink {
 public:
  using DrawLog = std::function<void(const DrawReport&)>;
  using CopyLog = std::function<void(const CopyReport&)>;
  using ErrorLog = std::function<void(const std::string&)>;
  using DisplayList = std::function<const uint8_t*(uint32_t addr, uint32_t size)>;

  Renderer3ds() : fifo_(this) {}
  ~Renderer3ds() { shutdown(); }
  Renderer3ds(const Renderer3ds&) = delete;
  Renderer3ds& operator=(const Renderer3ds&) = delete;

  // `mem`: guest memory (outlives the renderer). `target`: the screen target (240 x 400 RGBA8 + depth, C3D_RenderTargetSetOutput'ed by the caller). C3D_Init has been called.
  bool init(const RendererOptions& o, GuestMem* mem, C3D_RenderTarget* target);
  void shutdown();
  bool ready() const { return ready_; }

  // ---- game side
  size_t process(const uint8_t* data, size_t size) { return fifo_.process(data, size); }   // one complete command stream (FIFO bytes, display lists)
  uint32_t primitiveRunLimit() const override {
    if(!ready_||!opt_.batchMaxVertices)return 0;
    if(!evs_.empty()&&evs_.back().kind==Ev::Draw&&lastDrawGen_==fifo_.stateGeneration()&&gxTriCount(evs_.back().d)!=0)
      return opt_.batchMaxVertices-std::min<uint32_t>(opt_.batchMaxVertices,evs_.back().d.v.count);
    return opt_.batchMaxVertices;
  }
  GxFifo& fifo() { return fifo_; }
  void setFogPreparation(bool on) { fogPreparation_ = on; }
  void setPlanReuse(bool on) { planReuse_ = on; }
  void setPlanDelta(bool on, bool verify) { planDelta_ = on; planDeltaVerify_ = verify; planPrevOwner_.reset(); }   // (Claude v87)
  void setFrameTiming(bool on){opt_.frameTiming=on;}
  void setPlanTemplates(bool on){opt_.planTemplates=on;if(on && !planTemplates_)planTemplates_=std::make_unique<C3dPlanTemplateCache>(256u<<10);if(!on)planTemplates_.reset();}
  void setPacketWorker(bool on);
  void setPacketWorkerVerify(bool on){opt_.packetWorkerVerify=on;}
  void stopPacketWorker();
  bool packetWorkerActive() const{return packetWorker_&&packetWorker_->active();}
  void setDirectPack(bool on) { opt_.directPack=on; }
  void setStateNotes(bool on) { stateNotes_ = on; }
  // Retained geometry (gxretain.h): batches whose GX draws have the inputs of a batch of the previous frame are not decoded or packed again. Exact.
  void setRetainGeometry(bool on);
  void setStateDecodeRepeat(int n) { stateRepeat_ = n; }
  void setPlanNotes(bool on) { opt_.planNotes=on; }
  void setVertexPoolFifo(bool on) { vertexPoolFifo_=on; }
  void setStatePool(bool on) { statePoolEnabled_=on; }
  void setStateInPlace(bool on) { stateInPlace_ = on; }   // decode a new batch's state straight into its event (exact; saves a 3 KB temporary + copy)   // benchmark only: decode each batch's state n extra times (measures its cost without tick overhead)
  bool retainGeometry() const { return retainOn_; }
  GxRetainStats takeRetainStats() { return retainer_.takeStats(); }
  const PackedGeometryCache& packCache() const { return packCache_; }   // false: decodeState skips the diagnostic GxState::notes (only packets with packetNotes read them)
  void setPackedUniforms(bool on) { opt_.translate.packedUniforms = on; }
  void setDrawTimingEnabled(bool on) { drawTimingEnabled_ = on; }
  void setDisplayListResolver(DisplayList f) { dl_ = std::move(f); }
  void invalidateTextures(uint32_t addr, uint32_t size) { cache_.invalidateRange(addr, size); }   // guest memory written (loads, DMA)
  void invalidateAllTextures() { cache_.invalidateAll(); }
  size_t framesReady() const { return framesReady_; }

  // ---- material bundle (bundle.h)
  // Translating a material takes about a second on the ARM11 (55 ms on a PC), so the game ships the translator's results, made offline by tools/mkbundle_main.cpp, and loads them
  // before the first frame. Merges the file into the lookup; refused whole (false, `err` says why) when it is damaged or was made by other translator sources, in which case materials
  // are translated at run time as before. May be called more than once (the base bundle, then the extension a previous run recorded).
  bool loadMaterialBundle(const uint8_t* data, size_t size, std::string* err = nullptr);
  // Keep every translation the bundle lacked (effects built by game code rather than from its assets) so that missBundle() can write them for the next offline run.
  void recordBundleMisses(bool on);
  std::vector<uint8_t> missBundle() const { return misses_.serialize(sourceFingerprint()); }
  size_t missEntries() const { return misses_.translations() + misses_.approximations(); }
  // merges an earlier session's misses into the recorder too, so that writing missBundle() keeps them
  bool loadMissBundle(const uint8_t* data, size_t size, std::string* err = nullptr) { return misses_.load(data, size, sourceFingerprint(), err); }
  size_t bundleEntries() const { return bundle_.translations(); }

  // ---- render side
  // Draws the oldest finished frame (events up to and including its GXCopyDisp). force: if none is finished, draw whatever is recorded as one frame (tests, tools).
  FrameReport renderFrame(bool force = false);
  // Frame skipping (E, for a game that runs ahead of the renderer). The next renderFrame consumes its frame without presenting it, exactly for
  // everything later frames can observe: every palette load and every EFB -> texture copy happens, and with it every draw before the frame's last
  // copy (the copies capture them); only the draws after the last copy, which reach nothing but the unpresented picture, are skipped. The screen
  // output is detached for that partial frame, so the previous picture stays on screen. Not honoured (the frame is rendered and presented in full,
  // FrameReport::skipped false) when the frame's XFB copy does not clear the EFB: the next frame would then start from this frame's pixels.
  // One-shot: applies to the next rendered frame only.
  void setSkipNextFrame(bool on = true) { skipNext_ = on; }
  void swapRecording(RecordedFrame3ds& frame); // owner-thread only; no GPU calls

  // ---- logging and statistics
  void setLogs(DrawLog d, CopyLog c, ErrorLog e) { drawLog_ = std::move(d); copyLog_ = std::move(c); errLog_ = std::move(e); }
  // Called inside renderFrame right after C3D_FrameBegin returned: the GPU has finished the previous frame, the colour buffer holds its picture and nothing of this frame has been
  // queued yet (screenshots, read-back in tests).
  void setFrameBeginHook(std::function<void()> f) { beginHook_ = std::move(f); }
  size_t dumpTextures(std::FILE* f, uint64_t captureFrame);
  // (Claude) offline texture pack (texpack.h, built on the PC by texpack/mktexpack): texture cache misses read converted textures from it.
  // false: no usable pack (missing file, other version or conversion options). verify: convert anyway and compare (texPackStats().packVerifyBad).
  bool loadTexturePack(const char* path, bool verify, bool splitStore=false);
  const TextureStore::Stats& textureStoreStats()const{return texStore_.stats();}
  TexCacheStats texPackStats() const { return cache_.stats(); }
  size_t texturePackEntries() const { return texPack_.count(); }   // (Claude v91) SELECT: the textures used this or the previous frame (renderer3ds.cpp)
  // Called after cached guest textures have been verified, before new frame commands.
  void setCacheVerifiedHook(std::function<void()> f) { cacheVerifiedHook_ = std::move(f); }
  // Debugging: draws (index within the frame) for which this returns true are not drawn.
  void setDrawFilter(std::function<bool(int)> f) { skipDraw_ = std::move(f); }
  // Debugging: called with every finished plan (the draw's index within the frame) before it is drawn; the hook may change it (hashes are recomputed after).
  void setPlanTweak(std::function<void(C3dPlan&, int)> f) { tweak_ = std::move(f); }
  // Debugging: may edit a copy of the draw (state and geometry) before it is compiled.
  void setMatrixBatchProfile(bool on){ opt_.matrixBatchProfile=on; }
  void setMatrixBatch(bool on) { opt_.matrixBatch=on; }
  void setDrawReuse(bool on, size_t budgetBytes = 2560u << 10);   // (Claude v73) drawreuse3ds.h; false frees every entry
  void setDrawReplay(bool on, size_t budgetBytes = 2048u << 10) { replayOn_ = on; replayBudget_ = budgetBytes; if (!on) { replayPrev_.clear(); replayCur_.clear(); } }   // (Claude v117)
  size_t drawReplayBytes() const { return replayBytes_; }   // approximate when read from another thread
  void setDrawReplayBudget(size_t b) { replayBudget_ = b; }   // any thread; above it no entries are added, and the next frame drops what is left of the last one
  bool drawReplay() const { return replayOn_; }
  void computeReplayKeys() { for (auto& e : evs_) computeReplayKey(e); }   // (Claude v121) bench: what the FIFO worker does in the game
  void setDrawRecord(bool on) { recordOn_ = on; }   // (Claude v119) replay hits sent as recorded command words (Backend3ds::drawRecorded)
  bool drawRecord() const { return recordOn_; }
  std::shared_ptr<DirectDrawRegistry> directRegistry()const{return directRegistry_;}
  bool directSnapshots_=false;
 public:
  void setDirectSnapshots(bool on){directSnapshots_=on;}
  void setDirectGroups(bool on,bool cold=false){directGroups_=on;directGroupCold_=cold;}
  std::shared_ptr<DirectGroupRegistry> groupRegistry()const{return directGroupRegistry_;}
  void setDirectEvents(bool on,size_t slots=128){directRegistry_->configure(slots);directEvents_=on;}
  void setModelPrograms(bool on){modelPrograms_=on;be_.setCommandBlocks(on||commandBlocks_);}
  void setUniformDelta(bool on){uniformDelta_=on;}
  void setCommandBlocks(bool on){commandBlocks_=on;be_.setCommandBlocks(on||modelPrograms_);}
  void setRigidClip(bool on){rigidClip_=on;}
  void setFlushAllAtEnd(bool on) { flushAllAtEnd_ = on; }   // (Claude v118) true: C3D_FrameEnd(0), the whole linear heap cleaned each frame (old)
  bool flushAllAtEnd() const { return flushAllAtEnd_; }
  void setResidentGeometry(bool on, size_t budgetBytes = 2048u << 10);   // (Claude v86) resident packed geometry; false frees every entry (GPU idle)
  void setBatchMaxVertices(uint32_t n) { opt_.batchMaxVertices = n; }
  void setDrawEdit(std::function<void(GxState&, GxDraw&, int)> f) { edit_ = std::move(f); }
  Backend3ds& backend() { return be_; }
  TextureCache& cache() { return cache_; }
  C3dShaderCache& shaders() { return shaders_; }
  DrawCompileCache& drawCache() { return drawCache_; }
  const FrameConfig& frameConfig() const { return opt_.frame; }
  uint64_t framesRendered() const { return frames_; }

  // GxSink
  void onDraw(const GxRegs& r, const GxDraw& d) override;
  GxDraw* appendTarget(const GxRegs& r, const GxDraw& header) override;   // vertices are decoded straight into the batch (no copy)
  void onAppended(const GxRegs& r, GxDraw& into, uint32_t firstVertex) override;
  bool retainOffer(const GxDrawInputs& in, GxDraw& D) override;
  void retainDecoded(const GxDrawInputs& in, const GxDraw& D, uint32_t firstVertex, size_t firstTri, bool arrayError) override;
  void onCopy(const GxRegs& r) override;
  void onTlutLoad(const GxRegs& r) override;
  void onUiRoute(const GxRegs& r, const GxUiRoute& route) override;
  // (Claude v80) bottom-screen UI routing (n3ds ui_route_3ds.h). On: draws between a Route begin and end event render to a 320x240 bottom-screen target
  // through the affine map src -> dst (FitMode::Explicit; scissors map with it), with their own alpha / depth. The bottom target is made on first use and
  // linked to GFX_BOTTOM while routed draws keep arriving; `owner` is called with true when it takes the bottom screen and false when it gives it back
  // (after kRouteIdleFrames frames without a routed draw). Off: Route events are ignored (the draws stay on the top target, as before).
  void setUiRoute(bool on, std::function<void(bool)> owner = {}) { routeOn_ = on; routeOwner_ = std::move(owner); }
  bool uiRouteOwnsBottom() const { return routeLinked_; }
  C3D_RenderTarget* uiRouteTarget() const { return routeLinked_ ? routeTarget_ : nullptr; }   // diagnostics (read in the frame-begin hook)
  void onError(const std::string& e) override { if (errLog_) errLog_(e); }
  const uint8_t* resolveDisplayList(uint32_t addr, uint32_t size) override { return dl_ ? dl_(addr, size) : nullptr; }

 private:
  using Ev = RecordedEvent3ds;
  struct SnapMem : GuestMem {             // answers reads of one address range from a snapshot
    uint32_t addr = 0;
    const std::vector<uint8_t>* bytes = nullptr;
    const uint8_t* read(uint32_t a, size_t n) const override {
      if (!bytes || a < addr || uint64_t(a) + n > uint64_t(addr) + bytes->size()) return nullptr;
      return bytes->data() + (a - addr);
    }
  };

  bool routeTarget();                     // (Claude v80) makes / links the bottom target; false: unavailable (the draw stays on the top target)
  bool routeFrame(const GxState& st, FrameConfig& cfg) const;   // the Explicit frame config of a routed draw; false: degenerate mapping
  void drawOne(Ev& x, const Options& opt, int index, FrameReport& R, const matrixbatch::Group* group=nullptr,const DrawPacket* prepared=nullptr,std::shared_ptr<const PicaDraw> known={},std::shared_ptr<PackedUniforms> rows={});
  void closeRetained();
  void recycleEvent(Ev& e);
  void recycleEvents(size_t n, bool xfb);  // the first n events were consumed: their draws' vectors back to the pool, the events dropped                   // the open batch (the last Draw event) receives no more draws
  int stateMaxPm(int maxPm) const { return retainOn_ ? maxPm : kScanPosMtx; }
  bool ensureScratch();                   // creates the scratch target on first use (VRAM: a second colour + depth buffer); false when it cannot
  void copyOne(const GxCopy& c, const GxState& st, const EfbCopyOptions& co, int index, bool xfbClear, FrameReport& R);
  // (Claude v106) a copy to a registered synthesized texture (n3dsRegisterTexWarp) renders it instead of copying the EFB; true: handled
  bool warpOne(const GxCopy& c);

  RendererOptions opt_;
  // (Claude v80) UI routing state (render owner only)
  static constexpr uint32_t kRouteIdleFrames = 30;
  bool routeOn_ = false, routeFailed_ = false, routeLinked_ = false, routeActive_ = false;
  std::function<void(bool)> routeOwner_;
  GxUiRoute routeCur_;
  C3D_RenderTarget* routeTarget_ = nullptr;   // 240 x 320 RGBA8 + depth (rotated like the top target)
  C3D_RenderTarget* curTarget_ = nullptr;     // the target draws go to now (deferred switches)
  uint32_t routeIdle_ = 0;
  const FrameConfig* drawCfg_ = nullptr;      // drawOne's frame config (opt_.frame unless routed)
  bool drawRouted_ = false;
  MaterialBundle bundle_, misses_;
  GuestMem* mem_ = nullptr;
  C3D_RenderTarget* target_ = nullptr;
  bool ready_ = false;
  Backend3ds be_;
  TextureCache cache_;
  TexPackReader texPack_;
  std::FILE* texPackFile_ = nullptr;
  TextureStore texStore_;
  C3dShaderCache shaders_;
  DrawCompileCache drawCache_;
  C3dPlan planScratch_;
  std::unique_ptr<C3dPlanTemplateCache> planTemplates_;
  bool planReuse_ = false;
  // (Claude v87) plan delta: planScratch_ holds the full plan of event planPrevAt_ (frame planPrevFrame_) made from planPrevOwner_'s compiled draw
  bool planDelta_ = false, planDeltaVerify_ = false, planPrevRouted_ = false;
  size_t curEvAt_ = ~size_t(0), planPrevAt_ = ~size_t(0);
  uint64_t planPrevFrame_ = ~0ull;
  std::shared_ptr<const PicaDraw> planPrevOwner_;
  bool stateNotes_ = true;
  bool fogPreparation_ = false;
  FogCompileCache fogCompileCache_;
  bool drawTimingEnabled_ = true;
  bool useDrawCache_ = true;
  TexEntry view_;                         // the main target's colour buffer as a texture (the copy blits sample it)
  C3D_RenderTarget* scratch_ = nullptr;   // the first pass of two-pass draws renders here (same size and formats as the main target)
  TexEntry scratchView_;                  // ... and the second pass samples its colour buffer through this
  bool scratchFailed_ = false;
  GxFifo fifo_;
  DisplayList dl_;
  DrawLog drawLog_;
  CopyLog copyLog_;
  ErrorLog errLog_;
  std::function<void()> beginHook_;
  std::function<void()> cacheVerifiedHook_;
  std::function<bool(int)> skipDraw_;
  std::function<void(C3dPlan&, int)> tweak_;
  std::function<void(GxState&, GxDraw&, int)> edit_;
  EventDeque3ds evs_;
  std::vector<PVec> directRows_;
  std::unique_ptr<PacketWorker3ds> packetWorker_;
  DrawPacket packet_;                     // drawOne builds into this one packet (buildDrawInto): its vectors and uniform map keep their capacity
  size_t drawPoolRead_=0;
  bool vertexPoolFifo_=false;
  uint32_t matrixSamplePhase_=0;
  size_t drawPoolBytes_ = 0;              // vector capacity held by drawPool_ (bounded: see kDrawPoolBytes in renderer3ds.cpp)
  std::vector<GxDraw> drawPool_;          // the vertex vectors of drawn events, emptied with their capacity kept: new batches reuse them (no regrowth)
  bool appendNew_ = false;                // appendTarget made a new Draw event (onAppended decodes its state)
  bool retainOn_ = false;
  int stateRepeat_ = 0;
  StateVectorPool3ds statePool_;
  bool statePoolEnabled_=false;
  bool stateInPlace_ = false;
  bool skipNext_ = false;
  GxRetainer retainer_;
  PackedGeometryCache packCache_;
  bool skipped_ = false;                  // retainOffer skipped the draw being appended; skipMaxPm_ is its maxPosMtx
  int skipMaxPm_ = -1;
  uint64_t lastDrawGen_ = ~0ull;          // fifo_.stateGeneration() when the last Draw event was recorded
  size_t framesReady_ = 0;
  std::unique_ptr<Ev> pendingClear_;      // the previous frame's GXCopyDisp, whose clear this frame starts with
  uint64_t frames_ = 0;
  bool reuseOn_ = false;                  // (Claude v73) cross-frame draw reuse (drawreuse3ds.h)
  size_t reuseBudget_ = 0, reuseBytes_ = 0;
  std::unordered_map<uint64_t, std::unique_ptr<reuse::Entry>> reuse_;
  reuse::Key reuseKey_;
  std::unordered_set<uint64_t> reuseSeenPrev_, reuseSeenCur_;   // lookups that missed in the previous / this frame: only a draw seen twice is kept
  void reuseEvict(bool all);
  bool reuseEvictUnused();
  uint64_t reusePressureEvicts_=0;
  // (Claude v86) translate-once, resident geometry (submit.h ResidentGeometryKey): packed buffers of retained geometry kept in linear memory across frames
  struct ResidentEntry { ResidentGeometryKey key; void* mem = nullptr; size_t bytes = 0; uint64_t lastFrame = 0; ResidentGeometryHit hit; };
  // (Claude v117) draw replay: a draw whose retained geometry (id), draw state (GxDrawState) and every other plain GxState field equal those of the
  // draw at the same place in the previous frame reuses that draw's finished packet + C3D plan: no compile lookup, packet fill, plan or finalize.
  // Per hit: the resident geometry entry is re-checked and touched, each texture unit is looked up again (same entry required), and the uniforms
  // are packed again when the uniform block differs. Matrix groups, routed / two-pass / non-resident draws take the normal path.
  struct ReplayEntry;
  using ReplayMap=std::unordered_multimap<uint64_t,std::shared_ptr<ReplayEntry>>;
  struct ReplayEntry {
    ReplayMap::iterator location;uint64_t availableFrame=~0ull,drawnFrame=~0ull;
    std::shared_ptr<const UniformRecipe> recipe;
    uint64_t geoId = 0;
    GxDrawState draw;
    std::vector<uint8_t> tail;                          // GxState bytes [kcolor, notes)
    uint64_t rk[2] = {0, 0};                            // (Claude v121) replay key of the draw it was made from (0,0: none)
    uint64_t tailHash = 0;                              // (Claude v119) replayTailHash(tail): the projection is not in it
    std::shared_ptr<const VtxUniforms> uniformBlock;    // identity of the uniforms the packet holds (kept alive: no address reuse)
    std::shared_ptr<const CompactUniforms> uniformSections;
    DrawPacket packet;                                  // own packed uniforms; geometry = resident memory
    C3dPlan plan;                                       // built from `packet`
    uint64_t residentHash = 0; void* residentMem = nullptr; ResidentGeometryKey residentKey;
    std::weak_ptr<ResidentEntry> residentLease;
    ResidentEntry* residentPtr = nullptr; uint64_t residentGen = ~0ull;   // (Claude v122) valid while Renderer3ds::residentGen_ is unchanged
    // (Claude v122) a matrix group's draw (groupCount > 1): the members' geometry ids, slot maps and the palette size it was built with
    bool directGroupPublished=false;
    uint32_t groupCount = 0, groupPalette = 0; std::vector<uint64_t> groupIds; std::vector<std::array<int8_t, 10>> groupSlots;
    // (Claude v117) the plan's packed uniform rows, compacted (the packet keeps no PackedUniforms: 5 KB each): plan.uniforms[i].rows point here
    struct RowMap { uint8_t uniform, array; uint16_t first, count, at; };
    std::unique_ptr<PVec[]> rows; std::vector<RowMap> rowMap;
    Backend3ds::RecordedDraw rec;                       // (Claude v119) its command words
    size_t bytes = 0; size_t* live = nullptr;          // accounted in Renderer3ds::replayBytes_
    ~ReplayEntry() { if (live) *live -= bytes; }
  };
  size_t replayBytes_ = 0; std::atomic<size_t> replayBudget_{2048u << 10};
  std::unique_ptr<PackedUniforms> replayScratch_;      // repacking a hit's uniforms
  bool replayOn_ = false;
  bool flushAllAtEnd_ = false;
  bool recordOn_ = true;
  bool directGroupCold_=false;
  bool directGroups_=false;
  std::shared_ptr<DirectGroupRegistry> directGroupRegistry_=std::make_shared<DirectGroupRegistry>();
  void learnDirectGroup(const std::shared_ptr<ReplayEntry>&,const Options&);
  bool drawDirectGroup(const Options&,int,FrameReport&,size_t&);
  bool directEvents_=false;
  std::shared_ptr<DirectDrawRegistry> directRegistry_=std::make_shared<DirectDrawRegistry>();
  void learnDirect(Ev&,const std::shared_ptr<ReplayEntry>&);
  void drawDirect(Ev&,const Options&,int,FrameReport&);
  bool commandBlocks_=false,uniformDelta_=false,modelPrograms_=false;
  bool rigidClip_=false;
  RigidClipPlanes rigidClipPlanes_;
  bool outsideRigidClip(const Ev& x);
  ReplayMap replayPrev_, replayCur_;   // by retained geometry id
  size_t replayCursor_ = 0;
  bool replayNoInsert_ = false;
  bool replayGroups_ = true;   // (Claude v122) matrix groups replay too (N3DS_REPLAY_GROUPS=0: off)   // this draw's matrices change every frame (2D): no entry
  // (Claude v122) a matrix group's identity: its members' geometry ids and slot maps (which palette entry each member matrix uses) and the palette size
  struct ReplayGroupKey { const uint64_t* ids = nullptr; const std::array<int8_t, 10>* slots = nullptr; size_t count = 0, palette = 0; };
  bool replayTry(Ev& use, FrameReport& R, DrawReport& D, const ReplayGroupKey* group = nullptr, Backend3ds::RecordedItem* prepared = nullptr,std::shared_ptr<ReplayEntry>* resolved=nullptr);
  struct ReplayProgram {std::array<std::weak_ptr<ReplayEntry>,8> entries;uint8_t count=0,segmentEnd[8]={};};
  struct GroupPrograms {std::array<std::weak_ptr<ReplayEntry>,4> entries;uint8_t next=0;};
  std::unordered_map<uint64_t,GroupPrograms> modelGroupPrograms_;
  bool executeGroupProgram(int index,FrameReport&,size_t& consumed);
  std::unordered_map<uint64_t,ReplayProgram> modelProgramsCache_;
  std::vector<std::shared_ptr<const UniformRecipe>> uniformRecipes_; // <=128 unique recipes; <=80KiB
  bool executeModelProgram(int index,FrameReport&,size_t& consumed);
  void learnModelProgram(const std::shared_ptr<ReplayEntry>*,size_t count);
  bool replayEvent(Ev& x, int index, FrameReport& R,size_t* consumed=nullptr);
  bool replayGroupEvent(Ev& x, const matrixbatch::Group& group, int index, FrameReport& R,const Options&);   // (Claude v122)
  bool replayInsert(const GxState& st, uint64_t geoId, const uint64_t* rk, const ReplayGroupKey* group = nullptr,std::shared_ptr<ReplayEntry>* inserted=nullptr);
  bool residentOn_ = false;
  size_t residentBudget_ = 0, residentBytes_ = 0;
  std::unordered_map<uint64_t, std::shared_ptr<ResidentEntry>> resident_;
  std::unordered_set<uint64_t> residentSeenPrev_, residentSeenCur_;
  uint64_t residentGen_ = 0;   // (Claude v122) bumped whenever a resident entry is erased or replaced
  ResidentGeometryLookup residentLookup_;
  static bool residentFind(void* self, uint64_t hash, const ResidentGeometryKey& key, ResidentGeometryHit& out);
  void residentEvict(bool all);
  bool residentEvictUnused();
  void updateLinearPressure();
};

}  // namespace t2p
