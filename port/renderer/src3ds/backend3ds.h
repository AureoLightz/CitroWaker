#include "texture_binding_key.h"
// citro3d executor for draw plans (c3dplan.h). 3DS only: needs libctru + citro3d. Everything that decides *what* to send lives in the plan (host-tested); this file only
// replays it through citro3d and owns the GPU-side objects (loaded shader programs, uploaded textures, the per-frame linear vertex/index arena).
//
// Frame protocol:  C3D_FrameBegin -> backend.beginFrame() -> backend.beginTarget(target) -> any number of draw(plan) -> C3D_FrameEnd.
//   The command buffer (C3D_Init size) holds one whole frame: use at least 1 MB. draw() hands finished parts of it to the GPU every quarter of the buffer
//   (C3D_FrameSplit with the command list flushed, arena flushed first) so the GPU starts early, and refuses draws once the buffer is 97% full.
//   C3D_FrameBegin returns only after the GPU finished the previous frame's command list, so the arena can be reused from the start of every frame.
//   beginTarget() = C3D_FrameDrawOn + forgets cached state (citro3d resets the viewport, and with it the scissor, on a new target).
//
// Texture lifetime: TextureCache entries carry the C3D_Tex in TexEntry::user. draw() uploads entries that have none; hook TextureCache::setEvictCallback to release().
//   release() does not free at once: the entry may still be referenced by draws already in this frame's command list (an invalidation or a newer EFB copy at the same
//   address can evict an entry that was just drawn with), so the C3D_Tex is freed by the second beginFrame() after the release (C3D_FrameBegin returns after the GPU
//   finished the previous frame's list).
//
// EFB copies (efbcopy.h): setMainView() once per main target, then copy() for every planned copy inside the frame. A copy switches targets (C3D_FrameDrawOn: citro3d
//   flushes the colour buffer being left, which is what makes the main target's pixels visible to the blit) and returns to the main target before it draws the clear.
#pragma once
#include <3ds.h>
#include <citro3d.h>

#include <functional>
#include <string>
#include <utility>
#include <unordered_map>
#include <memory>
#include <vector>

#include "c3dplan.h"
#include "prepared_draw.h"
#include "efbcopy.h"
#include "multiplan.h"

namespace t2p {

class Backend3ds {
 public:
  void setKickDraws(int n){kickDraws_=n>0&&n<4096?n:0;}   // (Claude v120) start the GPU every n draws (C3Dx_FrameKick); 0 = at frame end only
  int kickDraws() const { return kickDraws_; }
  void setSplitStep(float f){splitStep_=f>0.005f&&f<=0.5f?f:0.25f;}   // (Claude) smaller steps start the GPU earlier in the frame; output unchanged
  void setTextureBindingMemo(bool on){textureBindingMemo_=on;}
  const void* textureLevel0(const TexEntry& e, size_t& bytes, int& width, int& height) const;   // (Claude v91) diagnostics: the GPU copy of an entry
  using Log = std::function<void(const std::string&)>;

  struct Stats {
    uint32_t draws = 0, copyAllocFails = 0, rtLive = 0, rtVramBytes = 0, refused = 0, noArena = 0, noCmdBuf = 0, splits = 0, kicks = 0;
    uint32_t shadersLoaded = 0, texturesUploaded = 0;
    uint64_t textureBytes = 0, arenaPeak = 0;
    uint32_t stateSkips = 0;          // attribute / texenv / effect groups not re-sent because their hash matched
    uint32_t copies = 0, clears = 0;  // EFB copies blitted into textures / copy-clear fills drawn
    uint32_t twoPass = 0;             // two-pass draws completed (drawTwoPass)
    uint32_t viewRepacks = 0;         // (Claude v100) padded-view repacks (GX TextureCopy) before a copy blit / second pass
  };

  Backend3ds() = default;
  ~Backend3ds() { shutdown(); }
  Backend3ds(const Backend3ds&) = delete;
  Backend3ds& operator=(const Backend3ds&) = delete;

  void setLog(Log l) { log_ = std::move(l); }
  bool init(size_t arenaBytes = 6u << 20);   // linear memory for the vertex + index data of one frame, and a placeholder texture
  void shutdown();

  void beginFrame();                         // right after C3D_FrameBegin
  // outside a frame (before C3D_FrameBegin): deletes pooled render textures beyond `keep`. citro3d panics (svcBreak) when a render
  // target is deleted inside a frame, so expired rendered copies go to a pool that allocRendered reuses, and only this trims it
  size_t rtPoolSize() const { return rtPool_.size(); }
  void trimRenderPool(size_t keep = 12);
  void beginTarget(C3D_RenderTarget* t);     // C3D_FrameDrawOn and cache reset

  uint8_t* allocateTextureOutput(TexEntry&,int,int,int,int,const std::vector<size_t>&,size_t);
  void finishTextureOutput(TexEntry&);
  // Single-draw reservations are transactional until a draw consumes them.
  size_t geometryMark() const { return arenaUsed_; }
  bool reserveGeometry(size_t vertexBytes,size_t indexCount,uint8_t*&,uint16_t*&);
  void rewindGeometry(size_t mark);
  bool draw(C3dPlan& plan);                  // false: nothing was drawn (lastError)
  // (Claude v119) recorded draws (draw replay): the command words of a draw's state groups (attributes, buffers, effect + fog table, TEV,
  // TEV buffer, texture units, scissor) and of its draw call, made once by citro3d's own bind functions (citro3d/source/record.c) right
  // after draw() issued the same plan. drawRecorded sends only the groups whose state differs from what the GPU has (the same tests as
  // draw()), the uniforms through citro3d, and puts each sent group's state into citro3d's context so its later re-emissions stay right.
  struct RecordedMaterial {   // attributes, effect + fog table, TEV, TEV buffer: shared by every recorded draw with the same three plan hashes
    uint64_t hashAttrs = 0, hashTexEnv = 0, hashEffect = 0;
    std::vector<uint32_t> words; uint16_t at[5] = {};   // attributes [at0,at1) effect [at1,at2) TEV [at2,at3) TEV buffer [at3,at4)
    std::vector<uint8_t> state;                          // record.c C3Dx_Shared
  };
  struct UniformLayout {
    struct Row {uint8_t id,uniform,row;};
    std::vector<Row> rows;
  };
  struct RecordedDraw {
    bool prepared=false;
    bool ok = false;
    std::shared_ptr<const UniformLayout> layout;
    uint64_t serial = 0; // monotonic content generation, never a recycled address
    std::shared_ptr<const RecordedMaterial> mat;
    std::vector<uint32_t> words; uint16_t at[5] = {};   // buffers, texture units, scissor, draw call
    std::vector<uint8_t> state;                          // record.c C3Dx_Own
    const void* program = nullptr;
    uint8_t unitKey[3] = {}; TextureBindingKey sampler[3]; uint64_t gen[3] = {};
    PixelRect scissor;
    size_t bytes() const { return words.capacity() * 4 + state.capacity(); }   // the material is shared (materialBytes)
  };
  bool loadPreparedDraw(const char* path,bool verify=false);
  uint64_t preparedBuilds()const{return preparedBuilds_;}
  uint64_t preparedSubmits()const{return preparedSubmits_;}
  uint64_t preparedMismatches()const{return preparedMismatches_;}
  size_t materialBytes() const { return matBytes_; }
  bool recordDraw(const C3dPlan& L, RecordedDraw& R);   // right after draw(L) returned true; false: not recordable (R.ok false)
  bool drawRecorded(C3dPlan& L, RecordedDraw& R);       // = draw(L); falls back to draw(L) when R does not apply (then R.ok may turn false)
  uint32_t recordedDraws() const { return recordedDraws_; }
  struct RecordedItem { C3dPlan* plan=nullptr; RecordedDraw* record=nullptr; };
  // All-or-nothing eligibility. False has emitted no draws; caller replays singles.
  void setCommandBlocks(bool on){commandBlockEnable_=on;}
  int drawRecordedBlock(const RecordedItem* items,size_t count); // -1 refused; otherwise number drawn
  uint32_t commandBlocks()const{return commandBlocks_;}
  uint32_t commandBlockDraws()const{return commandBlockDraws_;}

  // Cold command-template adapter. Emits exactly C3Dx_Uniforms' GPU words without
  // mutating Citro3D uniform shadows or touching the live command list.
  struct UniformRowWords { uint8_t reg=0; uint32_t xyzw[4]={}; };
  static bool makeUniformCommands(const UniformRowWords* rows,size_t n,std::vector<uint32_t>& out);


  // Makes `view` (the TexEntry planEfbCopy() gets as mainView) a window onto `mainTarget`'s colour buffer: a width x height RGBA8 texture with the buffer's own
  // dimensions (240 x 400 for the top screen), sampled by the copy blits. The view owns nothing; call again when the target is recreated.
  bool setMainView(TexEntry& view, C3D_RenderTarget* mainTarget);
  // Performs a planned copy: registers its texture in `cache`, allocates it in VRAM with a render target, draws the blit into it, goes back to `mainTarget` and draws
  // the clear. false: not (completely) done (lastError); the texture is dropped from the cache again when its blit did not happen. Afterwards the main target is
  // bound and the state caches are invalid.
  bool copy(EfbCopyPlan& plan, TextureCache& cache, C3D_RenderTarget* mainTarget);
  // (Claude v106) synthesized mipmapped textures: allocate `levels` levels (RGBA8, a render target per level) for a rendered entry, and draw a plan
  // into one level (then back to mainTarget; state caches invalid afterwards)
  bool allocRenderedMips(TexEntry& e, int levels);
  bool drawIntoLevel(TexEntry& e, int level, C3dPlan& L, C3D_RenderTarget* mainTarget);
  // Two-pass draws (multiplan.h): switches to `scratch` (a render target of the main target's size with a depth buffer; the plan's scratchView is a window onto its colour buffer, made
  // with setMainView), clears its depth when the plan asks, draws the first pass, returns to `mainTarget` and draws the second pass, which reads the scratch back. false: not (completely)
  // drawn (lastError); the main target is bound afterwards either way and the state caches are invalid.
  bool drawTwoPass(C3dMultiPlan& plan, C3D_RenderTarget* scratch, C3D_RenderTarget* mainTarget);
  // The pixels of an uploaded or rendered texture as the GPU stores them (tiled, the PICA format of the entry), for read-back after the frame that drew them has
  // finished. false: the entry has no texture of its own.
  bool textureImage(const TexEntry& e, const void** data, uint32_t* bytes) const;
  void release(const TexEntry& e);           // texture cache eviction: frees the C3D_Tex
  void releaseShader(C3dShader& s);          // frees the loaded program (shutdown, or when the shader cache is cleared)

  const std::string& lastError() const { return err_; }
  static float cmdUsage();                   // fraction of the command buffer in use (live, unlike C3D_GetCmdBufUsage)
  const Stats& stats() const { return st_; }

 private:
  struct ShaderHandle;
  struct GpuTex;
  bool fail(const std::string& s);
  bool allocRendered(TexEntry& e);
  void refreshViews(C3dPlan& L);
  void drawIdOverride(C3dPlan& L);
 public:
  void beginDrawIdFrame(bool on);   // (Claude v101) DIAGNOSTIC: the frame's draws write their number as a colour ([drawid] log lines)
 private:
  void freeGpuTex(GpuTex* g);
  bool loadShader(C3dShader& s);
  bool uploadTexture(TexEntry& e);
  void* arenaAlloc(size_t bytes, size_t align);
  void invalidateCaches();
 public:
  void flushArena();   // (Claude v118) public: Renderer3ds flushes the arena before C3D_FrameEnd(GX_CMDLIST_FLUSH)
 private:

  Log log_;
  std::string err_;
  Stats st_;

  u8* arena_ = nullptr;
  size_t arenaSize_ = 0, arenaUsed_ = 0, arenaFlushed_ = 0;
  float splitAt_ = 0.25f;
  int kickDraws_ = 0, sinceKick_ = 0;   // (Claude v120) mid-frame GPU start
  float splitStep_ = 0.25f;   // (Claude) fraction of the command buffer between GPU hand-offs (setSplitStep)
 public:
  // (Claude v83) called when a texture finds no linear memory: frees memory nothing in flight uses (draw-reuse entries not used this frame) and returns
  // true when it freed some, so the allocation is tried once more instead of refusing the draw
  std::function<bool()> linearPressure;
 private:
  C3D_Tex dummy_;                            // bound to units 1 and 2 when unused (C3D_TexBind cannot unbind them)
  C3D_Tex slot_[3];                          // the textures citro3d is told about: shallow copies of the cached C3D_Tex with this draw's sampler state
  C3D_FogLut fogLut_;                        // citro3d keeps the pointer until the table is uploaded
  bool dummyOk_ = false;
  uint64_t frameNo_ = 0;
  std::vector<GpuTex*> rtPool_;   // rendered-copy textures (with their render target) no queued command list reads any more
  std::vector<std::pair<GpuTex*, uint64_t>> garbage_;   // released textures and the frame they were released in

  // what the GPU / citro3d currently has, to skip redundant work
  const ShaderHandle* program_ = nullptr;
  uint64_t attrHash_ = 0, texenvHash_ = 0, effectHash_ = 0;
  bool attrValid_ = false, texenvValid_ = false, effectValid_ = false;
  float uniformShadow_[96][4];
  bool uniformValid_[96];
  C3D_Tex* bound_[3] = {nullptr, nullptr, nullptr};
  uint8_t boundKey_[3] = {0, 0, 0}; // 0 unknown, 1 disabled, 2 enabled
  TextureBindingKey boundSampler_[3];
  bool textureBindingMemo_=true;
  PixelRect scissor_;
  bool scissorValid_ = false;
  uint64_t lastHashes_[3] = {}; const void* lastIndices_ = nullptr; uint32_t lastIndexCount_ = 0;   // (Claude v119) the plan draw() issued last
  uint32_t recordedDraws_ = 0;
  struct CommandBlock {
    std::vector<uint64_t> serials;
    std::vector<uint32_t> words;
    std::vector<uint16_t> patches; // one per x,y,z,w input word, in draw/register order
    std::vector<uint16_t> inputRows; // draw<<8 | uniform register
    uint32_t masks[7][3]={};
  };
  std::unique_ptr<PreparedCommands> preparedDraw_;bool preparedVerify_=false;
  uint64_t preparedBuilds_=0,preparedSubmits_=0,preparedMismatches_=0;
  bool commandBlockEnable_=false;
  std::vector<std::shared_ptr<const UniformLayout>> uniformLayouts_;
  std::vector<CommandBlock> commandBlocksCache_;
  size_t commandBlocksBytes_=0;
  uint64_t nextRecordedSerial_=1;
  uint32_t commandBlocks_=0,commandBlockDraws_=0;
  std::unordered_map<uint64_t, std::shared_ptr<RecordedMaterial>> mats_; size_t matBytes_ = 0;   // (Claude v119)
};

}  // namespace t2p
