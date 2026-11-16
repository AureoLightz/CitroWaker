#include "replay_budget.h"
#include "renderer3ds.h"
#include "packet_oracle3ds.h"
extern "C" void n3dsLog(const char* fmt, ...);
extern "C" void n3dsPresentPace(void) __attribute__((weak));   // (Claude v120) n3ds gpu_3ds.cpp

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

extern void (*g_r3dsTrace)(const char*, int);   // debugging: progress points inside a frame (texcache.cpp)
#define R3T(w, i) do { if (g_r3dsTrace) g_r3dsTrace(w, i); } while (0)
namespace t2p {
namespace {
// The pool of emptied draws keeps their vectors' capacity so that the next frame's batches do not regrow them. Each pooled draw keeps the largest
// size every attribute ever had, so an unbounded pool grows towards (draws x every attribute's peak): 12 MB at Outset, more than the 3DS heap
// has left. Bounded by bytes; a single vector above kKeepVectorBytes is dropped (large batches are few and regrow cheaply).
constexpr size_t kDrawPoolBytes = 1536u << 10, kKeepVectorBytes = 32u << 10;
template <class V> size_t capBytes(const V& v) { return v.capacity() * sizeof(typename V::value_type); }
template <class V> void trimVector(V& v) { v.clear(); if (capBytes(v) > kKeepVectorBytes) V().swap(v); }
size_t drawCapBytes(const GxDraw& d) {
  const GxVertices& W = d.v;
  size_t n = capBytes(W.pos) + capBytes(W.nrm) + capBytes(W.pmIdx) + capBytes(d.tris);
  for (int c = 0; c < 2; c++) n += capBytes(W.clr[c]);
  for (int t = 0; t < 8; t++) n += capBytes(W.tex[t]) + capBytes(W.texMtxIdx[t]);
  return n;
}
double msOf(u64 ticks) { return double(ticks) * 1000.0 / double(SYSCLOCK_ARM11); }
constexpr uint32_t kRouteClear = 0x10183000u;   // (Claude v80) RGBA: the bottom screen's navy behind the routed UI (alpha 0)
}  // namespace

bool Renderer3ds::init(const RendererOptions& o, GuestMem* mem, C3D_RenderTarget* target) {
  shutdown();
  if (!mem || !target) return false;
  opt_ = o;
  setPlanTemplates(o.planTemplates);
  opt_.frame.resolve();
  mem_ = mem;
  target_ = target;
  cache_ = TextureCache(o.texCache);
  drawCache_ = DrawCompileCache(o.drawCacheEntries ? o.drawCacheEntries : 1);
  drawCache_.setRecentKeyFastPath(o.materialCacheFastPath);
  useDrawCache_ = o.drawCacheEntries != 0;
  cache_.setEvictCallback([this](const TexEntry& e) { be_.release(e); });
  if(o.texCache.directEncode)cache_.setOutputAllocator(
    [this](TexEntry& e,int f,int w,int h,int levels,const std::vector<size_t>& offsets,size_t bytes){return be_.allocateTextureOutput(e,f,w,h,levels,offsets,bytes);},
    [this](TexEntry& e){be_.finishTextureOutput(e);});
  if (!be_.init(o.arenaBytes)) return false;
  view_ = TexEntry();
  if (!be_.setMainView(view_, target_)) { be_.shutdown(); return false; }
  evs_.clear();
  lastDrawGen_ = ~0ull;
  pendingClear_.reset();
  framesReady_ = 0;
  frames_ = 0;
  ready_ = true;
  if(opt_.packetWorker)setPacketWorker(true);
  return true;
}

bool Renderer3ds::loadMaterialBundle(const uint8_t* data, size_t size, std::string* err) {
  if (!bundle_.load(data, size, sourceFingerprint(), err)) return false;
  setBundleLookup(&bundle_);
  return true;
}

void Renderer3ds::recordBundleMisses(bool on) {
  if (on) setBundleRecorder(&misses_);
  else if (bundleRecorder() == &misses_) setBundleRecorder(nullptr);
}

void Renderer3ds::setPacketWorker(bool on){
  opt_.packetWorker=on;
  if(!on){stopPacketWorker();return;}
  if(ready_&&!packetWorker_){auto w=std::make_unique<PacketWorker3ds>();if(w->start(errLog_))packetWorker_=std::move(w);}
}
void Renderer3ds::stopPacketWorker(){if(packetWorker_){packetWorker_->stop();packetWorker_.reset();}}
// (Claude v73) entries unused for 30 frames go (the GPU finished with them long ago); all: every entry (the GPU must be idle)
void Renderer3ds::reuseEvict(bool all){
  for(auto it=reuse_.begin();it!=reuse_.end();){
    if(all || it->second->lastFrame+30<frames_){reuseBytes_-=it->second->bytes;linearFree(it->second->mem);it=reuse_.erase(it);}
    else ++it;
  }
}
// (Claude v83) texture memory first: entries this frame has not used (C3D_FrameBegin waited for the previous frame, so no queued list reads them) go
bool Renderer3ds::reuseEvictUnused(){
  bool freed=false;
  for(auto it=reuse_.begin();it!=reuse_.end();){
    if(it->second->lastFrame<frames_){reuseBytes_-=it->second->bytes;linearFree(it->second->mem);it=reuse_.erase(it);freed=true;++reusePressureEvicts_;}
    else ++it;
  }
  return freed;
}
// (Claude v86) resident geometry: same lifetime rules as draw reuse (entries unused for 30 frames go; under texture pressure every entry this frame has not
// used goes: C3D_FrameBegin waited for the previous frame's list)
void Renderer3ds::residentEvict(bool all){
  for(auto it=resident_.begin();it!=resident_.end();){
    if(all || it->second->lastFrame+30<frames_){residentBytes_-=it->second->bytes;linearFree(it->second->mem);it->second->mem=nullptr;it=resident_.erase(it);residentGen_++;}
    else ++it;
  }
}
bool Renderer3ds::residentEvictUnused(){
  bool freed=false;
  for(auto it=resident_.begin();it!=resident_.end();){
    if(it->second->lastFrame<frames_){residentBytes_-=it->second->bytes;linearFree(it->second->mem);it->second->mem=nullptr;it=resident_.erase(it);freed=true;residentGen_++;}
    else ++it;
  }
  return freed;
}
bool Renderer3ds::residentFind(void* self,uint64_t hash,const ResidentGeometryKey& key,ResidentGeometryHit& out){
  auto& R=*static_cast<Renderer3ds*>(self);
  auto it=R.resident_.find(hash);
  if(it==R.resident_.end() || !(it->second->key==key)) return false;
  it->second->lastFrame=R.frames_;out=it->second->hit;return true;
}
void Renderer3ds::updateLinearPressure(){
  if(!reuseOn_ && !residentOn_){be_.linearPressure=nullptr;return;}
  be_.linearPressure=[this]{const bool a=reuseOn_&&reuseEvictUnused();const bool b=residentOn_&&residentEvictUnused();return a||b;};
}
void Renderer3ds::setResidentGeometry(bool on,size_t budgetBytes){
  if(!on){residentEvict(true);residentOn_=false;updateLinearPressure();return;}
  const size_t freeLinear=linearSpaceFree(),keep=3072u<<10;
  residentBudget_=std::min(budgetBytes,freeLinear>keep?freeLinear-keep:0);
  residentOn_=residentBudget_>0;
  updateLinearPressure();
  if(errLog_)errLog_("resident-geometry "+std::string(residentOn_?"on":"off")+": budget "+std::to_string(residentBudget_>>10)+" KB of "+std::to_string(freeLinear>>10)+" KB free linear memory");
}
void Renderer3ds::setDrawReuse(bool on,size_t budgetBytes){
  if(!on){reuseEvict(true);reuseOn_=false;updateLinearPressure();return;}
  updateLinearPressure();
  const size_t freeLinear=linearSpaceFree(),keep=1536u<<10;
  reuseBudget_=std::min(budgetBytes,freeLinear>keep?freeLinear-keep:0);
  reuseOn_=reuseBudget_>0;
  updateLinearPressure();
  if(errLog_)errLog_("draw-reuse "+std::string(reuseOn_?"on":"off")+": budget "+std::to_string(reuseBudget_>>10)+" KB of "+std::to_string(freeLinear>>10)+" KB free linear memory");
}
void Renderer3ds::shutdown() {
  modelProgramsCache_.clear();modelGroupPrograms_.clear();uniformRecipes_.clear();
  stopPacketWorker();
  reuseEvict(true);
  planTemplates_.reset();
  if (bundleLookup() == &bundle_) setBundleLookup(nullptr);
  if (bundleRecorder() == &misses_) setBundleRecorder(nullptr);
  if (!ready_) return;
  if (scratch_) { be_.release(scratchView_); C3D_RenderTargetDelete(scratch_); scratch_ = nullptr; }
  scratchFailed_ = false;
  if (routeTarget_) {   // (Claude v80) the bottom screen goes back to its CPU owner
    if (routeLinked_) { C3D_RenderTargetDetachOutput(routeTarget_); routeLinked_ = false; if (routeOwner_) routeOwner_(false); }
    C3D_RenderTargetDelete(routeTarget_); routeTarget_ = nullptr;
  }
  routeFailed_ = false; routeActive_ = false; routeIdle_ = 0;
  shaders_.forEach([&](C3dShader& s) { be_.releaseShader(s); });
  be_.shutdown();
  evs_.clear();
  pendingClear_.reset();
  framesReady_ = 0;
  ready_ = false;
}

void Renderer3ds::setRetainGeometry(bool on) {
  if (on == retainOn_) return;
  closeRetained();
  retainOn_ = on;
  fifo_.setRetainOffers(on);
  if (!on) { retainer_.clear(); packCache_.clear(); }
}

void Renderer3ds::closeRetained() {
  if (!retainer_.open()) return;
  struct P { uint64_t t = t2pProfTicks(); ~P() { if (t2pProfEnabled()) g_t2pProf[4] += t2pProfTicks() - t; } } prof;
  if (!evs_.empty() && evs_.back().kind == Ev::Draw) {
    Ev& x = evs_.back();
    if (auto g = retainer_.closeBatch(x.d, x.maxPm)) x.geo = std::move(g);
  } else {
    GxDraw none; retainer_.closeBatch(none, -1);   // (cannot happen: the open batch is the last event)
  }
}

bool Renderer3ds::retainOffer(const GxDrawInputs& in, GxDraw& D) {
  int m = -1;
  const uint64_t t = t2pProfTicks();
  skipped_ = retainer_.offer(in, D, appendNew_, fifo_.processSerial(), m);
  if (t2pProfEnabled()) g_t2pProf[7] += t2pProfTicks() - t;
  skipMaxPm_ = m;
  return skipped_;
}

void Renderer3ds::retainDecoded(const GxDrawInputs& in, const GxDraw& D, uint32_t firstVertex, size_t firstTri, bool arrayError) {
  retainer_.decoded(in, D, firstVertex, firstTri, arrayError, fifo_.processSerial());
}

void Renderer3ds::onDraw(const GxRegs& r, const GxDraw& d) {
  if (!ready_) return;
  closeRetained();
  const uint64_t gen = fifo_.stateGeneration();
  if (opt_.batchMaxVertices && !evs_.empty() && evs_.back().kind == Ev::Draw && gen == lastDrawGen_ && canAppend(evs_.back().d, d, opt_.batchMaxVertices)) {
    Ev& x = evs_.back();
    const uint64_t ta = t2pProfTicks();
    const int before = x.maxPm, mine = maxPosMtx(d);
    appendDraw(x.d, d);
    if (t2pProfEnabled()) g_t2pProf[5] += t2pProfTicks() - ta;
    x.merged = x.d.commandCount;
    if (mine > before) {
      x.maxPm = mine; const uint64_t t = t2pProfTicks(); x.state() = decodeState(r, &x.d, stateNotes_); if (t2pProfEnabled()) g_t2pProf[3] += t2pProfTicks() - t; }   // more position matrices are indexed now (GxDrawState::numPosMtx)
    return;
  }
  const uint64_t t0 = t2pProfTicks();
  evs_.emplace_back();
  Ev& x = evs_.back();
  x.kind = Ev::Draw;
  x.d = d;
  x.merged = d.commandCount;
  x.maxPm = maxPosMtx(d);
  const uint64_t t1 = t2pProfTicks();
  x.state() = decodeState(r, &d, stateNotes_);
  if (t2pProfEnabled()) g_t2pProf[3] += t2pProfTicks() - t1;
  if (t2pProfEnabled()) g_t2pProf[4] += t1 - t0;

  lastDrawGen_ = gen;
}

GxDraw* Renderer3ds::appendTarget(const GxRegs&, const GxDraw& h) {
  if (!ready_) return nullptr;
  const uint64_t gen = fifo_.stateGeneration();
  if (opt_.batchMaxVertices && !evs_.empty() && evs_.back().kind == Ev::Draw && gen == lastDrawGen_ && canAppendHeader(evs_.back().d, h, opt_.batchMaxVertices)) {
    appendNew_ = false;
    return &evs_.back().d;
  }
  closeRetained();
  evs_.emplace_back();
  Ev& x = evs_.back();
  x.kind = Ev::Draw;
  if (!drawPool_.empty()) {
    const size_t at=vertexPoolFifo_?drawPoolRead_:drawPool_.size()-1;
    drawPoolBytes_-=std::min(drawPoolBytes_,drawCapBytes(drawPool_[at]));
    x.d=std::move(drawPool_[at]);
    if(vertexPoolFifo_){if(++drawPoolRead_==drawPool_.size()){drawPool_.clear();drawPoolRead_=0;}}
    else drawPool_.pop_back();
  }
  x.d.commandCount = h.commandCount; x.merged = h.commandCount; x.d.prim = h.prim; x.d.vatIdx = h.vatIdx; x.d.vertexCount = h.vertexCount; x.d.unsupportedPrim = h.unsupportedPrim; x.d.vcd = h.vcd; x.d.vat = h.vat;
  x.d.retained = false; x.d.retainedTris = 0;
  appendNew_ = true;
  lastDrawGen_ = gen;
  return &x.d;
}

extern "C" { uint64_t g_n3dsDecodeCount[2] = {}; uint64_t g_n3dsIndirectDraws = 0; uint64_t g_n3ds2dDraws = 0; uint64_t g_n3ds2dHash = 0; }   // (Claude v95 diag) state decodes: [0] new batch, [1] batch grew its matrix count
void Renderer3ds::onAppended(const GxRegs& r, GxDraw& into, uint32_t firstVertex) {
  Ev& x = evs_.back();
  const int mine = skipped_ ? skipMaxPm_ : maxPosMtxFrom(into, firstVertex);
  skipped_ = false;
  const uint64_t t = t2pProfTicks();
  if (appendNew_) {
    ++g_n3dsDecodeCount[0];
    x.maxPm = mine;
    for (int i = 0; i < stateRepeat_; i++) { volatile int sink = decodeState(r, &x.d, stateNotes_, stateMaxPm(x.maxPm)).numTevStages; (void)sink; }
    if (stateInPlace_) { if(statePoolEnabled_)statePool_.take(x.state()); decodeStateInto(x.state(), r, &x.d, stateNotes_, stateMaxPm(x.maxPm)); }   // x.state() is the new event's fresh GxState
    else x.state() = decodeState(r, &x.d, stateNotes_, stateMaxPm(x.maxPm));
  } else {
    x.merged = x.d.commandCount;
    if (mine > x.maxPm) { ++g_n3dsDecodeCount[1]; x.maxPm = mine; x.state() = decodeState(r, &x.d, stateNotes_, stateMaxPm(x.maxPm)); }   // more position matrices are indexed now (GxDrawState::numPosMtx)
  }
  if (t2pProfEnabled()) g_t2pProf[3] += t2pProfTicks() - t;
}

void Renderer3ds::onCopy(const GxRegs& r) {
  if (!ready_) return;
  closeRetained();
  evs_.emplace_back();
  Ev& x = evs_.back();
  x.c = decodeCopy(r);
  x.state() = decodeState(r, nullptr);
  x.kind = x.c.toXfb ? Ev::Xfb : Ev::TexCopy;
  if (x.c.toXfb) { framesReady_++; if (retainOn_) retainer_.endFrame(); }
}

void Renderer3ds::onUiRoute(const GxRegs&, const GxUiRoute& route) {
  if (!ready_) return;
  closeRetained();
  evs_.emplace_back();
  Ev& x = evs_.back();
  x.kind = Ev::Route;
  x.route = route;
}

void Renderer3ds::onTlutLoad(const GxRegs& r) {
  if (!ready_) return;
  closeRetained();
  evs_.emplace_back();
  Ev& x = evs_.back();
  x.kind = Ev::Tlut;
  x.tl = decodeTlutLoad(r);
  const size_t n = size_t(x.tl.entries) * 2;
  if (n) {
    const uint8_t* src = mem_->read(x.tl.srcAddr, n);
    if (src) x.tlutBytes.assign(src, src + n);
  }
}

namespace {
// N3DS_CALIBRATE=1 (env): times loops of known instruction counts once, reported through the error log. Tells how the emulator's clock charges
// instructions (a real ARM11 MPCore runs the ALU loop at about 1 cycle per instruction, the load loop slower), so emulated ms can be read against hardware.
void calibrate(const std::function<void(const std::string&)>& log) {
  static bool done = false;
  if (done || !log || !std::getenv("N3DS_CALIBRATE")) return;
  done = true;
  auto ms = [](u64 t) { return double(t) * 1000.0 / double(SYSCLOCK_ARM11); };
  u32 n = 10000000;
  u64 t0 = svcGetSystemTick();
  asm volatile("1: subs %0, %0, #1\n bne 1b" : "+r"(n) : : "cc");                    // 2 instructions per iteration
  u64 t1 = svcGetSystemTick();
  u32 m = 4000000, acc = 0;
  asm volatile("1: add %1, %1, %0\n eor %1, %1, %1, lsr #3\n add %1, %1, #7\n orr %1, %1, #1\n subs %0, %0, #1\n bne 1b" : "+r"(m), "+r"(acc) : : "cc");   // 6 per iteration
  u64 t2 = svcGetSystemTick();
  static u32 buf[4096];
  u32 k = 2000000; u32* p = buf;
  asm volatile("1: ldr r3, [%1]\n add r3, r3, #1\n str r3, [%1], #4\n tst %1, #0x3F00\n subeq %1, %1, #0x3F00\n subs %0, %0, #1\n bne 1b" : "+r"(k), "+r"(p) : : "r3", "cc", "memory");   // 7 per iteration, a load and a store
  u64 t3 = svcGetSystemTick();
  char b[256];
  std::snprintf(b, sizeof b, "calibration: 20M-instruction branch loop %.1f ms, 24M-instruction ALU loop %.1f ms, 14M-instruction load/store loop %.1f ms (804 MHz at 1 instruction/cycle: 24.9 / 29.9 / 17.4 ms)",
                ms(t1 - t0), ms(t2 - t1), ms(t3 - t2));
  log(b);
  (void)acc;
}
}  // namespace

void Renderer3ds::swapRecording(RecordedFrame3ds& frame) {
  evs_.swap(frame.events);
  directRows_.swap(frame.directRows);
  std::swap(statePool_,frame.statePool);
  drawPool_.swap(frame.drawPool);
  std::swap(drawPoolRead_,frame.drawPoolRead);
  std::swap(drawPoolBytes_,frame.drawPoolBytes);
  std::swap(framesReady_,frame.framesReady);
}

FrameReport Renderer3ds::renderFrame(bool force) {
  FrameReport R;
  const uint32_t matrixPhase=matrixSamplePhase_++;
  if (!ready_) return R;
  R3T("calibrate", 0);
  calibrate(errLog_);
  R3T("frame policy", 0);
  // the events of one frame: up to and including the first XFB copy
  size_t n = 0;
  bool xfb = false;
  while (n < evs_.size()) {
    const bool isXfb = evs_[n++].kind == Ev::Xfb;
    if (isXfb) { xfb = true; break; }
  }
  if (!xfb) {
    if (!force || evs_.empty()) return R;
    closeRetained();
    n = evs_.size();
  }

  const uint64_t sp0=opt_.frameTiming?svcGetSystemTick():0;
  // frame policy: one W-buffer range for every draw of the frame when anything fogs
  Options opt = opt_.translate;
  bool routeEvents=false;   // (Claude v80) the packet pipe builds packets ahead with the top frame config: not across Route events
  if(routeOn_)for(size_t i=0;i<n;i++)if(evs_[i].kind==Ev::Route){routeEvents=true;break;}
  if(fogPreparation_) {
    FrameFogRange range;
    for(size_t i=0;i<n;i++)if(evs_[i].isDraw())range.add(evs_[i].viewState().draw);
    opt.frameFogRange=range.value();opt.fogCompileCache=&fogCompileCache_;
  } else {
    std::vector<GxDrawState> states;
    for(size_t i=0;i<n;i++)if(evs_[i].isDraw())states.push_back(evs_[i].viewState().draw);
    opt.frameFogRange=frameFogRange(states.data(),states.size());
  }
  if (g_picaForceZDepth) opt.frameFogRange = 0;   // (Claude v93) depth_z experiment: no W-buffer frames
  EfbCopyOptions co;
  co.frameFogRange = opt.frameFogRange;
  R.fogRange = opt.frameFogRange;

  // frame skipping: execute events [0, runUntil) in full; after it only palette loads and the XFB copy (see setSkipNextFrame)
  const bool skip = skipNext_ && xfb && evs_[n - 1].c.clear;
  skipNext_ = false;
  size_t runUntil = n;
  if (skip) {
    runUntil = 0;
    for (size_t i = 0; i < n; i++) if (evs_[i].kind == Ev::TexCopy) runUntil = i + 1;
    R.skipped = true;
  }
  if (skip && runUntil == 0 && !(pendingClear_ && pendingClear_->c.clear)) {   // nothing later frames can see needs the GPU: palette loads only, the XFB copy (its clear) stays pending
    for (size_t i = 0; i < n; i++) {
      Ev& x = evs_[i];
      if (x.kind == Ev::Tlut) { SnapMem snap; snap.addr = x.tl.srcAddr; snap.bytes = &x.tlutBytes; cache_.tlut().load(x.tl, snap); R.tlutLoads++; }
      else if (x.isDraw()) R.gxDraws += x.merged;
      else if (x.kind == Ev::Xfb) pendingClear_.reset(new Ev(std::move(x)));
    }
    recycleEvents(n, xfb);
    R.rendered = true;
    return R;
  }
  // partial frame: the screen keeps the previous picture
  const bool detach = skip && target_->linked;
  const gfxScreen_t outScreen = target_->screen; const gfx3dSide_t outSide = target_->side; const u32 outFlags = target_->transferFlags;
  if (detach) C3D_RenderTargetDetachOutput(target_);

  if(opt_.frameTiming)R.splitTicks[0]+=svcGetSystemTick()-sp0;
  // (Claude v80) the bottom screen goes back to its CPU owner after kRouteIdleFrames frames without a routed draw (or when routing was switched off).
  // Outside a frame: C3D_RenderTargetDetachOutput waits for the previous frame's queue (its transfers and swaps), as C3D_FrameBegin would.
  if (routeLinked_ && (!routeOn_ || routeIdle_ >= kRouteIdleFrames)) {
    C3D_RenderTargetDetachOutput(routeTarget_); routeLinked_ = false;
    if (routeOwner_) routeOwner_(false);
  }
  R3T("trimRenderPool", 0);
  be_.trimRenderPool();                  // render targets may only be deleted outside a frame
  R3T("C3D_FrameBegin", 0);
  const uint64_t beginTick=opt_.frameTiming?svcGetSystemTick():0;
  C3D_FrameBegin(opt_.frameFlags);       // blocks until the GPU has finished the previous frame
  if(opt_.frameTiming)R.beginWaitTicks=svcGetSystemTick()-beginTick;
  if (beginHook_) beginHook_();
  R3T("hook done", 0);
  be_.beginFrame();
  R3T("be.beginFrame", 0);
  const uint64_t sp1=opt_.frameTiming?svcGetSystemTick():0;
  cache_.beginFrame(*mem_);
  if(opt_.frameTiming)R.splitTicks[1]+=svcGetSystemTick()-sp1;
  if(planTemplates_ && matrixPhase%60==0)planTemplates_->purgeExpired();
  if(reuseOn_){reuseEvict(false);reuseSeenPrev_.swap(reuseSeenCur_);reuseSeenCur_.clear();}
  if(residentOn_){residentEvict(false);residentSeenPrev_.swap(residentSeenCur_);residentSeenCur_.clear();}
  if(replayOn_){replayPrev_.swap(replayCur_);replayCur_.clear();// Budget pressure used to discard EVERY recorded draw, even when only a few
    // entries needed reclaiming. Preserve validated survivors; miss handling and
    // the existing memory limit remain unchanged.
    const size_t replayLimit=replayBudget_.load(std::memory_order_relaxed);
    trimReplayBudget(replayPrev_,replayBytes_,replayLimit,64u<<10);
    if(modelPrograms_||directEvents_)for(auto it=replayPrev_.begin();it!=replayPrev_.end();++it){it->second->location=it;it->second->availableFrame=frames_;}
    replayCursor_=0;R.replayKb=uint32_t(replayBytes_>>10);}   // (Claude v117) draw replay: last frame's draws, in order
  if (cacheVerifiedHook_) cacheVerifiedHook_();
  R3T("cache.beginFrame", 0);
  if (!pendingClear_) {
    const uint32_t c = (uint32_t(opt_.initialClear[0]) << 24) | (uint32_t(opt_.initialClear[1]) << 16) | (uint32_t(opt_.initialClear[2]) << 8) | opt_.initialClear[3];
    C3D_RenderTargetClear(target_, C3D_CLEAR_ALL, c, picaFrameClearDepth());
  }
  if (routeLinked_) {                    // (Claude v80) while the bottom screen is ours, each frame presents it (cleared; the routed draws come later)
    C3D_RenderTargetClear(routeTarget_, C3D_CLEAR_ALL, routeEvents?kRouteClear:0, picaFrameClearDepth());
    C3D_FrameDrawOn(routeTarget_);       // marks it used: C3D_FrameEnd transfers it
  }
  be_.beginTarget(target_);
  curTarget_ = target_;
  routeActive_ = false;
  FrameConfig routeCfg;

  int drawIdx = 0, copyIdx = 0;
  if (pendingClear_) {                   // what the previous GXCopyDisp cleared (the picture it presented has left with the last C3D_FrameEnd)
    std::unique_ptr<Ev> p = std::move(pendingClear_);
    if (p->c.clear) copyOne(p->c, p->state(), co, copyIdx++, true, R);
    R3T("xfb clear", 0);
  }
  const bool packetPipe=packetWorkerActive()&&!opt_.matrixBatch&&!opt_.directPack&&!retainOn_&&!edit_&&!skipDraw_&&!tweak_&&!g_t2pProfOn&&runUntil==n&&!routeEvents;
  size_t queuedPackets=0,queuedBatches=0,submittedBatch=0,consumedBatch=0;std::array<size_t,2> batchRemaining{};if(packetPipe)packetWorker_->resetStats();
  matrixbatch::Group group; // bounded scratch reused, not zeroed for every draw
  // (Claude v78) the previous event's compiled draw. An event made incrementally from the event just before it (RecordedEvent3ds::dirtyIn) with only
  // TEV colour registers, viewport/scissor, matrix memory or lights changed has the same GxDrawState apart from numPosMtx and texmapScaled, which are
  // compared: drawCache_.get would return this same compiled draw.
  size_t keyMemoAt=~size_t(0);std::shared_ptr<const PicaDraw> keyMemoDraw;int keyMemoPos=0;uint8_t keyMemoScaled=0;
  planPrevOwner_.reset();planPrevAt_=~size_t(0);   // (Claude v87) plan delta never crosses frames
  const uint64_t sp2=opt_.frameTiming?svcGetSystemTick():0;
  for (size_t i = 0; i < n; i++) {
    Ev& x = evs_.front();
    size_t consumed=1;
    if (i >= runUntil && x.kind != Ev::Tlut && x.kind != Ev::Xfb) { if (x.isDraw()) R.gxDraws += x.merged; }
    else switch (x.kind) {
      case Ev::Route:                    // (Claude v80) the target switch waits for the next draw that needs it (map objects follow one another)
        R.routeMarkers++;
        if (routeOn_) { routeActive_ = x.route.begin; if (routeActive_) routeCur_ = x.route; }
        break;
      case Ev::Tlut: {
        SnapMem snap;
        snap.addr = x.tl.srcAddr;
        snap.bytes = &x.tlutBytes;
        cache_.tlut().load(x.tl, snap);
        R.tlutLoads++;
        break;
      }
      case Ev::DirectDraw: {
        if(drawDirectGroup(opt,drawIdx,R,consumed)){for(size_t j=0;j<consumed;++j)R.gxDraws+=evs_[j].merged;drawIdx+=int(consumed);}
        else{R.gxDraws+=x.merged;drawDirect(x,opt,drawIdx++,R);}break;
      }
      case Ev::Draw: {
        if(rigidClip_ && !routeActive_ && !edit_ && !tweak_ && !skipDraw_ && x.geo && !x.state().proj.ortho && x.state().draw.numPosMtx==1 && x.state().draw.vtx.numPosMtx==1 && x.geo->maxPm<=0){
          ++R.clipTests;
          if(outsideRigidClip(x)){++R.clipCulled;++R.draws;R.gxDraws+=x.merged;++drawIdx;break;}
        }
        if (x.state().proj.ortho) g_n3ds2dDraws++;   // (Claude v106 diag) orthographic (2D: HUD, shadow casters, screen effects) draws
        static int s_2dHashOn = -1;
        if (s_2dHashOn < 0) { const char* e = getenv("N3DS_2D_HASH"); s_2dHashOn = (e && *e == '1') || getenv("N3DS_2D_DUMP") ? 1 : 0; }
        if (s_2dHashOn && x.state().proj.ortho) {   // (diag) N3DS_2D_HASH=1: hash of the orthographic draws' inputs per frame ([draws2d] hash: frame-to-frame repeat test)
          uint64_t h = g_n3ds2dHash ^ 0x9E3779B97F4A7C15ull;
          auto mix = [&h](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; } };
          const GxVertices& v = x.d.v;
          mix(&x.d.prim, 1); mix(&v.count, 4); mix(&x.d.retained, 1);
          if (v.pos.size()) mix(v.pos.data(), v.pos.size() * 4);
          if (v.clr[0].size()) mix(v.clr[0].data(), v.clr[0].size());
          if (v.tex[0].size()) mix(v.tex[0].data(), v.tex[0].size() * 4);
          for (int m = 0; m < 8; m++) if (x.state().tex[m].valid) mix(&x.state().tex[m].addr, 4);
          mix(x.state().kcolor, sizeof x.state().kcolor); mix(x.state().tevReg, sizeof x.state().tevReg);
          const VtxUniformView U = x.state().uniformView();
          mix(U.proj, sizeof U.proj); mix(U.matColor, sizeof U.matColor);
          if (v.hasPmIdx && v.pmIdx.size()) {
            uint16_t used = 0; for (size_t i = 0; i < v.pmIdx.size(); i++) used |= uint16_t(1u << std::min(9, v.pmIdx[i] / 3));
            mix(v.pmIdx.data(), v.pmIdx.size());
            for (int m = 0; m < 10; m++) if (used & (1u << m)) mix(U.pos[m], sizeof U.pos[m]);
          } else mix(U.pos[0], sizeof U.pos[0]);
          g_n3ds2dHash = h;
          {   // (diag) N3DS_2D_DUMP=<frame>: each orthographic draw's own hash on that frame and the next
            static int at = -2; if (at == -2) { const char* e = getenv("N3DS_2D_DUMP"); at = e ? atoi(e) : -1; }
            if (at > 0 && (cache_.frame() == uint64_t(at) || cache_.frame() == uint64_t(at) + 1)) {
              uint64_t q = 0xCBF29CE484222325ull;
              auto mq = [&q](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; i++) { q ^= b[i]; q *= 0x100000001B3ull; } };
              uint64_t parts[5] = {};
              q = 1; if (v.pos.size()) mq(v.pos.data(), v.pos.size() * 4); parts[0] = q;
              q = 1; if (v.clr[0].size()) mq(v.clr[0].data(), v.clr[0].size()); parts[1] = q;
              q = 1; if (v.tex[0].size()) mq(v.tex[0].data(), v.tex[0].size() * 4); parts[2] = q;
              q = 1; mq(x.state().kcolor, sizeof x.state().kcolor); mq(x.state().tevReg, sizeof x.state().tevReg); parts[3] = q;
              q = 1; mq(U.proj, sizeof U.proj); mq(U.pos[0], sizeof U.pos[0]); parts[4] = q;
              ::n3dsLog("[2d] f %llu v %u tex0 %08lx pos %08lx clr %08lx tc %08lx konst %08lx mtx %08lx\n", (unsigned long long)cache_.frame(), unsigned(v.count), (unsigned long)x.state().tex[0].addr,
                        (unsigned long)parts[0], (unsigned long)parts[1], (unsigned long)parts[2], (unsigned long)parts[3], (unsigned long)parts[4]);
            }
          }
        }
        if (x.state().draw.indirect) {   // (Claude v106 diag) draws whose indirect warp is dropped; N3DS_IND_LOG=1 names the first 64
          g_n3dsIndirectDraws += x.merged;
          static int on = -1, n = 0;
          if (on < 0) { const char* e = getenv("N3DS_IND_LOG"); on = e && *e == '1'; }
          if (on && n < 64) {
            n++;
            const GxDrawState& g = x.state().draw;
            char b[400]; int k = std::snprintf(b, sizeof b, "[ind-draw] stages %d ind %u iref %06lx w %06lx %06lx %06lx v %u tex", int(g.tev.stages.size()), unsigned(g.indRaw[3] >> 24), (unsigned long)(g.indRaw[3] & 0xFFFFFF),
                                               (unsigned long)g.indRaw[0], (unsigned long)g.indRaw[1], (unsigned long)g.indRaw[2], unsigned(x.d.vertexCount));
            for (int m = 0; m < 8 && k < 360; m++) if (x.state().tex[m].valid) k += std::snprintf(b + k, sizeof b - size_t(k), " %d:%08lx/%dx%d/f%d", m, (unsigned long)x.state().tex[m].addr, x.state().tex[m].width, x.state().tex[m].height, x.state().tex[m].fmt);
            ::n3dsLog("%s\n", b);
            if (n == 1 && mem_) for (int m = 0; m < 4; m++) {   // the first one's textures, raw (sdmc:/3ds/ww3ds/inddump_<texmap>.bin: u32 w, h, fmt, then GX bytes)
              const GxTexBinding& t = x.state().tex[m];
              if (!t.valid) continue;
              const size_t nb = gxImageBytes(t);
              const uint8_t* p = mem_->read(t.addr, nb);
              char fn[64]; std::snprintf(fn, sizeof fn, "sdmc:/3ds/ww3ds/inddump_%d.bin", m);
              if (FILE* f = p ? std::fopen(fn, "wb") : nullptr) { const uint32_t hd[3] = {uint32_t(t.width), uint32_t(t.height), uint32_t(t.fmt)}; std::fwrite(hd, 4, 3, f); std::fwrite(p, 1, nb, f); std::fclose(f); }
            }
          }
        }
        // (Claude v80) UI route: this draw goes to the bottom target through the route's affine map (deferred target switch)
        bool routed=false;
        if(routeActive_) {
          if(!routeFrame(x.state(),routeCfg)) { R.routeRefused+=x.merged; R.gxDraws+=x.merged; break; }   // degenerate mapping: nothing to draw on either screen
          routed=routeTarget();          // false: no bottom target, the draw stays on the top screen (as with routing off)
        }
        {C3D_RenderTarget* want=routed?routeTarget_:target_;
         if(curTarget_!=want){be_.beginTarget(want);curTarget_=want;R.routeSwitches++;}}
        drawCfg_=routed?&routeCfg:&opt_.frame;drawRouted_=routed;
        if(!skipDraw_ && !(packetPipe&&!x.geo) && replayEvent(x,drawIdx,R,&consumed)){for(size_t j=0;j<consumed;++j)R.gxDraws+=evs_[j].merged;drawIdx+=int(consumed);drawCfg_=nullptr;drawRouted_=false;break;}   // (Claude v117)
        group.clear();
        std::shared_ptr<const PicaDraw> original;   // (Claude v75) the compiled draw the grouping looked up: drawOne builds with it (no second lookup)
        const uint64_t mbt0=opt_.frameTiming?svcGetSystemTick():0;
        if(opt_.matrixBatch && !skipDraw_ && !edit_ && !tweak_ && useDrawCache_) {   // (Claude v83) retained events group too: the group reads their shared geometry
          const bool sample=opt_.matrixBatchProfile && (((R.matrixAttempts++ + matrixPhase)&7)==0);
          const uint64_t mt0=sample?svcGetSystemTick():0;
          {auto& MP=matrixbatch::prof();MP.on=sample;MP.clock=[]()->uint64_t{return svcGetSystemTick();};}
          // (Claude v75) copy the key (GxDrawState holds vectors: allocations per draw) only when the flag drawOne adds changes it; the group members
          // are compared with the event's own key, as before
          const uint8_t scaled=cache_.scaledRenderedTexmaps(x.state().tex);
          GxDrawState scaledKey;
          if(scaled&~x.state().draw.texmapScaled){scaledKey=x.state().draw;scaledKey.texmapScaled|=scaled;}
          const GxDrawState& key=(scaled&~x.state().draw.texmapScaled)?scaledKey:x.state().draw;
          {
            constexpr uint32_t kSameKey=kStateDirtyTevReg|kStateDirtyView|kStateDirtyXfMtx|kStateDirtyLight;
            const uint8_t finalScaled=uint8_t(x.state().draw.texmapScaled|scaled);
            if(i==keyMemoAt+1 && keyMemoDraw && (x.dirtyIn&~kSameKey)==0 && x.state().draw.numPosMtx==keyMemoPos && finalScaled==keyMemoScaled) original=keyMemoDraw;
            else original=drawCache_.get(key,opt);
            keyMemoAt=i;keyMemoDraw=original;keyMemoPos=x.state().draw.numPosMtx;keyMemoScaled=finalScaled;
          }
          const uint64_t ms1=opt_.frameTiming?svcGetSystemTick():0;
          if(opt_.frameTiming){R.matrixSplit[0]+=ms1-mbt0;const uint64_t c1=svcGetSystemTick();R.matrixSplit[3]+=c1-ms1;}
          uint64_t ms2=0;
          const uint64_t mt1=sample?svcGetSystemTick():0;
          uint64_t mt2=mt1;
          if(group.append(x.state(),x.geo?x.geo->d:x.d,*original,opt,0xFFFFFFFFu,x.geo?&x.geo->pmMemo:nullptr)==matrixbatch::MB_OK) {
            for(size_t j=1;i+j<n && i+j<runUntil && j<matrixbatch::Group::MaxMembers;j++) {
              auto& next=evs_[j];
              if(next.kind!=Ev::Draw || (packetPipe && next.geo) || group.append(next.state(),next.geo?next.geo->d:next.d,*original,opt,packetPipe?0xFFFFFFFFu:next.dirtyIn,next.geo?&next.geo->pmMemo:nullptr)!=matrixbatch::MB_OK)break;   // (Claude v78) the packet pipe edits queued states' texmapScaled: no dirty shortcut
            }
            mt2=sample?svcGetSystemTick():0;
            if(opt_.frameTiming){ms2=svcGetSystemTick();R.matrixSplit[1]+=ms2-ms1;}
            if(group.count>1) {if(opt_.frameTiming){R.matrixSplit[4]++;R.matrixSplit[5]+=group.count;}
              // (Claude v105) palette sizes share shader variants in buckets (2..4 -> 4, 5..10 -> 10): every exact size compiled its own variant
              // (2-8 ms per compile on the console, ~85% of the compile misses while moving). Slots past the group's palette are never indexed.
              const size_t exactPal=group.paletteSize;
              const size_t bucketPal=exactPal<=1?exactPal:exactPal<=4?4:10;
              auto variantKey=key;variantKey.numPosMtx=int(bucketPal);variantKey.vtx.numPosMtx=1;
              std::shared_ptr<const PicaDraw> variant;
              bool okay=false;
              for(int attempt=0;attempt<2 && !okay;attempt++){
                if(attempt==1){ if(bucketPal==exactPal)break; variantKey.numPosMtx=int(exactPal); }
                variant=drawCache_.get(variantKey,opt);
                okay=variant->overall!=Fidelity::Unsupported && variant->vs.ok && !variant->pass2 && variant->vsUniformRegs<=96 && variant->attrs.size()<=12 &&
                  (exactPal==1 || variant->vs.usesMtxIdxAttr) && variant->vs.usesNormal==original->vs.usesNormal && variant->numUnits==original->numUnits &&
                  variant->tev.prog.stages.size()==original->tev.prog.stages.size();
                for(int u=0;u<3 && okay;u++)okay=variant->unitTexmap[u]==original->unitTexmap[u] && variant->unitTexcoord[u]==original->unitTexcoord[u];
              }
              if(okay) {
                auto uniforms=makeUniformBlock();
                const auto view=x.state().uniformView();
                std::memcpy(uniforms->proj,view.proj,sizeof uniforms->proj);
                std::memcpy(uniforms->pos,view.pos,size_t(view.poseCount)*sizeof uniforms->pos[0]);
                std::memcpy(uniforms->nrm,view.nrm,size_t(view.poseCount)*sizeof uniforms->nrm[0]);
                std::memcpy(uniforms->matColor,view.matColor,sizeof uniforms->matColor);
                std::memcpy(uniforms->ambColor,view.ambColor,sizeof uniforms->ambColor);
                std::memcpy(uniforms->light,view.light,sizeof uniforms->light);
                std::memcpy(uniforms->texMtx,view.texMtx,sizeof uniforms->texMtx);
                std::memcpy(uniforms->texScale,view.texScale,sizeof uniforms->texScale);   // (Claude v86) pooled
                for(size_t p=0;p<group.paletteSize;p++){
                  std::memcpy(uniforms->pos[p],group.palette[p].p,sizeof group.palette[p].p);
                  std::memcpy(uniforms->nrm[p],group.palette[p].n,sizeof group.palette[p].n);
                }
                x.state().uniformBlock=uniforms;x.state().draw=std::move(variantKey);consumed=group.count;original=std::move(variant);
              }
              if(opt_.frameTiming)R.matrixSplit[2]+=svcGetSystemTick()-ms2;
            }
          }
          if(sample){
            const uint64_t mt3=svcGetSystemTick();++R.matrixSamples;
            R.matrixTicks[0]+=mt1-mt0;R.matrixTicks[1]+=mt2-mt1;R.matrixTicks[2]+=mt3-mt2;matrixbatch::prof().on=false;
          }
        }
        if(opt_.frameTiming)R.splitTicks[5]+=svcGetSystemTick()-mbt0;
        for(size_t j=0;j<consumed;j++)R.gxDraws+=evs_[j].merged;
        if (skipDraw_ && skipDraw_(drawIdx)) { drawIdx++; break; }
        if(packetPipe&&!x.geo){
          // The rendered-copy map changes only at copy barriers; do not queue
          // across one. Deque references stay live until their packet is used.
          while(queuedBatches<2 && queuedPackets<n-i && queuedPackets<evs_.size()){
            std::array<PacketWorker3ds::Input,PacketWorker3ds::BatchSize> inputs{};
            size_t count=0,bytes=0;
            while(count<inputs.size()&&queuedPackets+count<n-i&&queuedPackets+count<evs_.size()){
              Ev& future=evs_[queuedPackets+count];if(future.kind!=Ev::Draw||future.geo)break;
              const size_t estimate=size_t(future.d.v.count)*128+future.d.tris.size()*sizeof(uint16_t);
              if(count&&bytes+estimate>(256u<<10))break;
              future.state().draw.texmapScaled|=cache_.scaledRenderedTexmaps(future.state().tex);
              inputs[count++]={&future.state(),&future.d};bytes+=estimate;
            }
            if(!count)break;
            packetWorker_->submitBatch(inputs.data(),count,opt,opt_.frame,useDrawCache_?&drawCache_:nullptr,opt_.packetNotes);
            batchRemaining[submittedBatch++&1]=count;++queuedBatches;queuedPackets+=count;
          }
          R3T("packet-worker wait",drawIdx);
          const DrawPacket& prepared=packetWorker_->acquire(drawTimingEnabled_);
          curEvAt_=~size_t(0);planPrevOwner_.reset();drawOne(x,opt,drawIdx,R,nullptr,&prepared);packetWorker_->release();--queuedPackets;++R.packetJobs;
          if(!--batchRemaining[consumedBatch&1]){++consumedBatch;--queuedBatches;}
        }else if(consumed>1 && !skipDraw_ && replayGroupEvent(x,group,drawIdx,R,opt)){   // (Claude v122) the whole matrix group from the previous frame
        }else{curEvAt_=i;drawOne(x,opt,drawIdx,R,consumed>1?&group:nullptr,nullptr,std::move(original));}
        drawIdx+=int(consumed);
        if(routed)for(size_t j=0;j<consumed;j++)R.routedDraws+=evs_[j].merged;
        drawCfg_=nullptr;drawRouted_=false;
        break;
      }
      case Ev::TexCopy: { if (routeActive_) R.routeCopies++; if (curTarget_ != target_) { be_.beginTarget(target_); curTarget_ = target_; R.routeSwitches++; }
        R3T("copy", copyIdx); const uint64_t c0=opt_.frameTiming?svcGetSystemTick():0; copyOne(x.c, x.state(), co, copyIdx++, false, R); if(opt_.frameTiming)R.splitTicks[3]+=svcGetSystemTick()-c0; R3T("copy done", copyIdx); break; }
      case Ev::Xfb: pendingClear_.reset(new Ev(std::move(x))); break;   // the frame ends here; its clear (if any) belongs to the next one
    }
    // Commands now own their vertex/uniform bytes. Release each consumed GX
    // event before the next texture conversion, rather than retaining a whole
    // rendered frame beside the producer's next frame and upload scratch.
    const uint64_t rct0=opt_.frameTiming?svcGetSystemTick():0;
    for(size_t j=0;j<consumed;j++){recycleEvent(evs_.front());
      const uint64_t pf=opt_.frameTiming?svcGetSystemTick():0;evs_.pop_front();if(opt_.frameTiming)R.splitTicks[7]+=svcGetSystemTick()-pf;}
    if(opt_.frameTiming)R.splitTicks[6]+=svcGetSystemTick()-rct0;
    i+=consumed-1;
  }
  if(opt_.frameTiming)R.splitTicks[2]+=svcGetSystemTick()-sp2;
  routeIdle_=R.routedDraws?0:std::min<uint32_t>(routeIdle_+1,kRouteIdleFrames);
  routeActive_=false;
  if(packetPipe)R.packetWaitTicks=packetWorker_->waitTicks();
  if(planTemplates_)R.templateStats=planTemplates_->stats();
  R.reuseBytes=reuseBytes_;R.reuseEntries=reuse_.size();R.residentBytes=residentBytes_;R.residentEntries=resident_.size();
  R3T("frameEnd", 0);
  if(n3dsPresentPace)n3dsPresentPace();   // (Claude v120) n3ds gpu_3ds.cpp: even display intervals (weak: absent in bench tools)
  const uint64_t endTick=opt_.frameTiming?svcGetSystemTick():0;
  // (Claude v118) every CPU write the GPU reads is flushed where it is made (draw arena below and at splits, resident / reuse geometry, C3D_TexFlush
  // after texture uploads and warps, the command list by GX_CMDLIST_FLUSH): C3D_FrameEnd(0) also cleaned all 18 MB of linear memory each frame
  if(flushAllAtEnd_)C3D_FrameEnd(0);
  else{be_.flushArena();C3D_FrameEnd(GX_CMDLIST_FLUSH);}
  if(opt_.frameTiming)R.endSubmitTicks=svcGetSystemTick()-endTick;
  R3T("frameEnd done", 0);
  const uint64_t sp4=opt_.frameTiming?svcGetSystemTick():0;
  if (detach) C3D_RenderTargetSetOutput(target_, outScreen, outSide, outFlags);
  recycleEvents(0, xfb);
  if(opt_.frameTiming)R.splitTicks[4]+=svcGetSystemTick()-sp4+R.endSubmitTicks;
  cache_.publishKnown();   // (Claude v102) the producer's snapshot skips textures this cache holds (texKnownContains)
  R3T("renderFrame out", 0);
  R.rendered = true;
  return R;
}

void Renderer3ds::recycleEvent(Ev& e) {
    if(drawPoolRead_){
      drawPool_.erase(drawPool_.begin(),drawPool_.begin()+std::ptrdiff_t(drawPoolRead_));
      drawPoolRead_=0;
    }
    if(statePoolEnabled_ && e.kind==Ev::Draw)statePool_.give(e.state());
    // (Claude v90) the pool vector never grows past 1024 entries (~370 KB of GxDraw). v89 crashed on the console at save load: the vector doubled
    // from 2048 to 4096 entries (one 1.5 MB reallocation) when the heap had 2 MB left, operator new failed and the move wrote through null.
    // Growth is also refused once the vector is full: an unpooled draw just frees its vectors (as at the byte cap below).
    constexpr size_t kDrawPoolEntries = 1024;
    if (e.kind != Ev::Draw || drawPool_.size() >= kDrawPoolEntries) return;
    if (drawPool_.size() == drawPool_.capacity() && drawPool_.capacity() < kDrawPoolEntries) drawPool_.reserve(kDrawPoolEntries);
    GxDraw& d = e.d;
    GxVertices& W = d.v;
    W.count = 0;
    trimVector(W.pos); trimVector(W.nrm); trimVector(W.pmIdx);
    for (int c = 0; c < 2; c++) trimVector(W.clr[c]);
    for (int t = 0; t < 8; t++) { trimVector(W.tex[t]); trimVector(W.texMtxIdx[t]); }
    trimVector(d.tris);
    const size_t bytes = drawCapBytes(d);
    if (!bytes) return;   // (Claude v104) a retained draw's vectors are empty: pooling it gives the producer nothing (was ~93% of pushes)
    if (drawPoolBytes_ + bytes > kDrawPoolBytes) return;
    drawPoolBytes_ += bytes;
    drawPool_.push_back(std::move(d));
}

// (Claude v91) record: "TXD1", u32 captureFrame, u32 addr, i32 gx w,h,fmt,levels,wrapS,wrapT,tlutFmt,needAlpha, u64 tlutHash, u64 fullHash,
// u64 lastUseFrame, i32 rendered, pica fmt, pica w, pica h, pica levels, u32 guestBytes, u32 picaBytes, guest bytes (current memory; zeros if unmapped), pica level 0
size_t Renderer3ds::dumpTextures(std::FILE* f, uint64_t captureFrame) {
  if (!f) return 0;
  size_t n = 0; const uint64_t now = cache_.frame();
  cache_.forEachEntry([&](const TextureCache::EntryInfo& k, const TexEntry& e) {
    if (e.lastUseFrame + 1 < now) return;
    size_t pb = 0; int pw = 0, ph = 0; const void* pd = be_.textureLevel0(e, pb, pw, ph);
    const uint8_t* guest = (!e.rendered && mem_ && e.srcBytes) ? mem_->read(k.addr, e.srcBytes) : nullptr;
    const uint32_t gb = e.rendered ? 0u : uint32_t(e.srcBytes);
    auto u32w = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto u64w = [&](uint64_t v) { std::fwrite(&v, 8, 1, f); };
    std::fwrite("TXD1", 1, 4, f); u32w(uint32_t(captureFrame)); u32w(k.addr);
    u32w(uint32_t(e.rendered ? e.gxW : k.width)); u32w(uint32_t(e.rendered ? e.gxH : k.height)); u32w(uint32_t(e.rendered ? e.gxFmt : k.fmt)); u32w(uint32_t(k.levels));
    u32w(uint32_t(k.wrapS)); u32w(uint32_t(k.wrapT)); u32w(uint32_t(k.tlutFmt)); u32w(k.needAlpha ? 1u : 0u);
    u64w(k.tlutHash); u64w(e.fullHash); u64w(e.lastUseFrame);
    u32w(e.rendered ? 1u : 0u); u32w(uint32_t(e.tex.fmt)); u32w(uint32_t(pw)); u32w(uint32_t(ph)); u32w(uint32_t(e.tex.levels));
    u32w(gb); u32w(uint32_t(pd ? pb : 0));
    static uint8_t zero[4096] = {};
    if (guest) std::fwrite(guest, 1, gb, f); else for (uint32_t o = 0; o < gb; o += sizeof zero) std::fwrite(zero, 1, std::min<uint32_t>(sizeof zero, gb - o), f);
    // rendered copies live in VRAM, which must never reach fwrite (v84: the FS process faults the kernel): copied through a heap buffer
    static uint8_t bounce[16384];
    // (Claude v101) RAM view: the CPU's cached copy of linear-memory texture data is dropped first, so the dump shows what the GPU reads
    // (v91-v100 dumps read through the cache and always matched the PC; the console still drew garbage texels)
    if (pd && !e.rendered) { const uintptr_t a = reinterpret_cast<uintptr_t>(pd); if (a < 0x1F000000u || a >= 0x1F600000u) GSPGPU_InvalidateDataCache(const_cast<void*>(pd), u32(pb)); }
    if (pd) for (size_t o = 0; o < pb; o += sizeof bounce) { const size_t c = std::min(sizeof bounce, pb - o);
      std::memcpy(bounce, static_cast<const uint8_t*>(pd) + o, c); std::fwrite(bounce, 1, c, f); }
    n++;
  });
  std::fflush(f);
  return n;
}

bool Renderer3ds::loadTexturePack(const char* path, bool verify, bool splitStore) {
  cache_.setPack(nullptr, false);
  if (texPackFile_) { std::fclose(texPackFile_); texPackFile_ = nullptr; }
  texStore_.close();
  if(splitStore){
    const auto& t=opt_.texCache;
    if(!texStore_.open(path)||!texPack_.open([this](uint64_t o,size_t n,void* d){return texStore_.read(o,n,d);},texPackOptionsSig(true,t.tryEtc1,t.fastLossyError))){texStore_.close();return false;}
    cache_.setPack(&texPack_,verify);return true;
  }
  texPackFile_ = std::fopen(path, "rb");
  if (!texPackFile_) return false;
  std::setvbuf(texPackFile_, nullptr, _IONBF, 0);   // records and whole textures are read in one piece: no stdio copy
  std::FILE* f = texPackFile_;
  const TexCacheOptions& t = opt_.texCache;
  if (!texPack_.open([f](uint64_t o, size_t n, void* d) { return std::fseek(f, long(o), SEEK_SET) == 0 && std::fread(d, 1, n, f) == n; },
                     texPackOptionsSig(true, t.tryEtc1, t.fastLossyError))) {
    std::fclose(texPackFile_); texPackFile_ = nullptr; return false;
  }
  cache_.setPack(&texPack_, verify);
  return true;
}

bool Renderer3ds::outsideRigidClip(const Ev& x) {
  const GxDraw& d=x.geo->d;
  if(d.unsupportedPrim||x.state().draw.vtx.dbgClipReg>=0)return false;
  const unsigned prim=d.prim&0xF8;
  if(prim!=0x80&&prim!=0x90&&prim!=0x98&&prim!=0xA0)return false;
  ClipBounds& b=x.geo->clipBounds;
  if(!b.state){
    b.state=-1;
    if(!d.v.hasPos||!d.v.count||d.v.pos.size()<size_t(d.v.count)*3||d.tris.empty())return false;
    for(int k=0;k<3;++k)b.lo[k]=b.hi[k]=d.v.pos[k];
    for(uint32_t i=0;i<d.v.count;++i)for(int k=0;k<3;++k){
      const float v=d.v.pos[size_t(i)*3+k];if(!std::isfinite(v)||std::fabs(v)>1e10)return false;
      b.lo[k]=std::min(b.lo[k],v);b.hi[k]=std::max(b.hi[k],v);
    }
    b.state=1;
  }
  if(b.state!=1)return false;
  const auto u=x.state().uniformView();
  // XY/w projection is shared by the world draws. Calling mapFrame for each
  // cull check would evict its depth-policy cache and repeat work at draw time.
  struct ProjectionMemo { bool valid=false;uint8_t cull=0;GxViewport viewport;FrameConfig config;float source[4][4],mapped[4][4]; };
  static thread_local ProjectionMemo memo;
  if(!memo.valid || memo.cull!=x.state().cullMode || std::memcmp(memo.source,u.proj,sizeof memo.source) ||
     std::memcmp(&memo.viewport,&x.state().viewport,sizeof memo.viewport) || std::memcmp(&memo.config,&opt_.frame,sizeof memo.config)){
    const auto f=mapFrame(x.state(),opt_.frame,PicaDepthMap{},widescreenXScale(x.state(),d,opt_.frame));
    if(!f.ok)return false;
    std::memcpy(memo.source,u.proj,sizeof memo.source);std::memcpy(memo.mapped,f.proj,sizeof memo.mapped);
    // Depth policy only modifies row2; no depth/near/far rejection is done here.
    std::memset(memo.mapped[2],0,sizeof memo.mapped[2]);
    std::memcpy(&memo.viewport,&x.state().viewport,sizeof memo.viewport);std::memcpy(&memo.config,&opt_.frame,sizeof memo.config);
    memo.cull=x.state().cullMode;memo.valid=true;
  }
  return rigidClipPlanes_.update(u.pos[0],memo.mapped)&&rigidClipPlanes_.outside(b)!=0;
}

void Renderer3ds::recycleEvents(size_t n, bool xfb) {
  if(directSnapshots_){directRegistry_->publishFrame();directGroupRegistry_->publishFrame();}
  for (size_t i = 0; i < n; i++) recycleEvent(evs_[i]);
  evs_.erase(evs_.begin(), evs_.begin() + std::ptrdiff_t(n));
  if (xfb && framesReady_) framesReady_--;
  if (retainOn_) packCache_.endFrame();
  frames_++;
}

// (Claude v80) bottom-screen UI routing
bool Renderer3ds::routeTarget() {
  if (routeFailed_) return false;
  if (!routeTarget_) {
    routeTarget_ = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!routeTarget_) { routeFailed_ = true; if (errLog_) errLog_("ui route: no bottom target (VRAM)"); return false; }
  }
  if (!routeLinked_) {
    // the display transfer converts to the bottom framebuffer's format (GSP and GX transfer format numbers agree)
    const u32 out = u32(gfxGetScreenFormat(GFX_BOTTOM)) & 7;
    C3D_RenderTargetSetOutput(routeTarget_, GFX_BOTTOM, GFX_LEFT, GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(out) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    routeLinked_ = true;
    C3D_RenderTargetClear(routeTarget_, C3D_CLEAR_ALL, kRouteClear, picaFrameClearDepth());   // later frames clear it right after C3D_FrameBegin
    if (routeOwner_) routeOwner_(true);
  }
  return true;
}

// The affine map of a route: the source rectangle (the draws' 2D projection input space) through the draw's projection and viewport gives EFB pixels;
// the output rectangle then puts that EFB rectangle onto the destination (bottom-screen pixels). Scissors and the viewport map with it (mapFrame).
bool Renderer3ds::routeFrame(const GxState& S, FrameConfig& cfg) const {
  const GxUiRoute& r = routeCur_;
  if (r.src[2] <= 0 || r.src[3] <= 0 || r.dst[2] <= 0 || r.dst[3] <= 0) return false;
  const float (*P)[4] = S.uniformView().proj;
  const GxViewport& V = S.viewport;
  float ex[2], ey[2];
  for (int k = 0; k < 2; k++) {
    const float x = float(r.src[0]) + (k ? float(r.src[2]) : 0.0f), y = float(r.src[1]) + (k ? float(r.src[3]) : 0.0f);
    const float cx = P[0][0] * x + P[0][1] * y + P[0][3], cy = P[1][0] * x + P[1][1] * y + P[1][3], w = P[3][0] * x + P[3][1] * y + P[3][3];
    if (!(std::fabs(w) > 1e-6f)) return false;
    ex[k] = V.x + V.width * 0.5f * (1.0f + cx / w);
    ey[k] = V.y + V.height * 0.5f * (1.0f - cy / w);
  }
  const float eW = ex[1] - ex[0], eH = ey[1] - ey[0];
  if (!(eW > 1e-3f) || !(eH > 1e-3f)) return false;   // a mirrored mapping is not a UI route
  cfg = opt_.frame;
  cfg.targetW = 320; cfg.targetH = 240; cfg.rotate = true; cfg.widescreen = false; cfg.fit = FitMode::Explicit;
  const float sx = float(r.dst[2]) / eW, sy = float(r.dst[3]) / eH;
  cfg.outW = float(cfg.efbW) * sx; cfg.outX = float(r.dst[0]) - ex[0] * sx;
  cfg.outH = float(cfg.efbH) * sy; cfg.outY = float(r.dst[1]) - ey[0] * sy;
  return std::isfinite(cfg.outW) && std::isfinite(cfg.outH) && std::isfinite(cfg.outX) && std::isfinite(cfg.outY);
}

bool Renderer3ds::ensureScratch() {
  if (scratch_) return true;
  if (scratchFailed_) return false;
  scratch_ = C3D_RenderTargetCreate(target_->frameBuf.width, target_->frameBuf.height, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
  if (!scratch_ || !be_.setMainView(scratchView_, scratch_)) {
    if (scratch_) { C3D_RenderTargetDelete(scratch_); scratch_ = nullptr; }
    scratchFailed_ = true;
    return false;
  }
  return true;
}

// (Claude v87) N3DS_PLAN_DELTA_VERIFY: every field a delta plan executes with, against a full build of the same packet; nullptr when equal
static const char* samePlanDelta(const C3dPlan& a,const C3dPlan& b){
  if(a.ok!=b.ok)return "ok";
  if(a.shader!=b.shader)return "shader";
  const C3dAttrs &A=a.attrs,&B=b.attrs;
  if(A.count!=B.count||A.permutation!=B.permutation||A.stride!=B.stride||A.planar!=B.planar)return "attrs";
  for(int i=0;i<A.count;i++)if(A.loader[i].reg!=B.loader[i].reg||A.loader[i].format!=B.loader[i].format||A.loader[i].count!=B.loader[i].count||A.offset[i]!=B.offset[i]||A.size[i]!=B.size[i])return "attr loader";
  if(a.uniforms.size()!=b.uniforms.size())return "uniform count";
  for(size_t i=0;i<a.uniforms.size();i++){const C3dUniform &u=a.uniforms[i],&v=b.uniforms[i];
    if(u.name!=v.name||u.id!=v.id||u.count!=v.count)return "uniform decl";
    if(u.count&&std::memcmp(u.rows,v.rows,sizeof(PVec)*size_t(u.count)))return "uniform rows";}
  if(a.texenv.used!=b.texenv.used||a.texenv.updateBuffer!=b.texenv.updateBuffer||a.texenv.bufferColor!=b.texenv.bufferColor)return "texenv";
  for(int i=0;i<6;i++){const HwStage &s=a.texenv.stage[i],&t=b.texenv.stage[i];
    if(s.sources!=t.sources||s.modifiers!=t.modifiers||s.ops!=t.ops||s.constColor!=t.constColor||s.scales!=t.scales)return "texenv stage";}
  for(int u=0;u<3;u++){const C3dUnit &s=a.units[u],&t=b.units[u];
    if(s.enabled!=t.enabled||s.entry!=t.entry||s.projective!=t.projective||s.wrapS!=t.wrapS||s.wrapT!=t.wrapT||s.magFilter!=t.magFilter||s.minFilter!=t.minFilter||
       s.mipFilter!=t.mipFilter||s.minLevel!=t.minLevel||s.maxLevel!=t.maxLevel||s.lodBias!=t.lodBias)return "unit";}
  const C3dEffect &E=a.fx,&F=b.fx;
  if(E.zBuffer!=F.zBuffer||E.zScale!=F.zScale||E.zOffset!=F.zOffset||E.cull!=F.cull||E.depthTest!=F.depthTest||E.depthFunc!=F.depthFunc||E.writeMask!=F.writeMask||
     E.alphaTest!=F.alphaTest||E.alphaFunc!=F.alphaFunc||E.alphaRef!=F.alphaRef||E.logicOp!=F.logicOp||E.logic!=F.logic||E.eqRgb!=F.eqRgb||E.eqAlpha!=F.eqAlpha||
     E.srcRgb!=F.srcRgb||E.dstRgb!=F.dstRgb||E.srcAlpha!=F.srcAlpha||E.dstAlpha!=F.dstAlpha||E.blendColor!=F.blendColor||E.fog!=F.fog||E.fogColor!=F.fogColor)return "effect";
  if(E.fog&&std::memcmp(a.fogLut,b.fogLut,sizeof a.fogLut))return "fog lut";
  if(a.scissor.x0!=b.scissor.x0||a.scissor.y0!=b.scissor.y0||a.scissor.x1!=b.scissor.x1||a.scissor.y1!=b.scissor.y1)return "scissor";
  if(a.vertices!=b.vertices||a.vertexBytes!=b.vertexBytes||a.indices!=b.indices||a.indexCount!=b.indexCount)return "geometry";
  if(a.hashAttrs!=b.hashAttrs||a.hashTexEnv!=b.hashTexEnv||a.hashEffect!=b.hashEffect)return "hashes";
  if(a.linearGeometry!=b.linearGeometry)return "linear";
  return nullptr;
}

// (Claude v117) draw replay
namespace {
inline const uint8_t* replayTailBegin(const GxState& s) { return reinterpret_cast<const uint8_t*>(&s.kcolor); }
inline size_t replayTailBytes(const GxState& s) { return size_t(reinterpret_cast<const uint8_t*>(&s.notes) - replayTailBegin(s)); }
// (Claude v119) byte equality of two word-aligned ranges, 8 words a step (newlib's memcmp orders as well as compares)
static_assert(offsetof(GxState, kcolor) % 4 == 0 && (offsetof(GxState, proj) - offsetof(GxState, kcolor)) % 4 == 0 && sizeof(GxProjection) % 4 == 0, "replay tail words");
inline bool replayEq(const uint8_t* x, const uint8_t* y, size_t n) {
  const uint32_t* a = reinterpret_cast<const uint32_t*>(x); const uint32_t* b = reinterpret_cast<const uint32_t*>(y);
  size_t w = n / 4;
  for (; w >= 8; w -= 8, a += 8, b += 8)
    if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) | (a[4] ^ b[4]) | (a[5] ^ b[5]) | (a[6] ^ b[6]) | (a[7] ^ b[7])) return false;
  for (; w; w--, a++, b++) if (*a != *b) return false;
  return std::memcmp(a, b, n & 3) == 0;
}
// (Claude v119) the tail without the projection, word-wise (two lanes); only a prefilter: equal hashes are confirmed byte for byte
inline uint64_t replayTailHash(const uint8_t* t, size_t n) {
  const size_t p0 = offsetof(GxState, proj) - offsetof(GxState, kcolor), p1 = p0 + sizeof(GxProjection);
  uint32_t a = 0x811C9DC5u, b = 0x9E3779B9u;
  auto run = [&](size_t from, size_t to) {
    size_t i = from;
    for (; i + 8 <= to; i += 8) { uint32_t w0, w1; std::memcpy(&w0, t + i, 4); std::memcpy(&w1, t + i + 4, 4); a = (a ^ w0) * 0x01000193u; b = (b ^ w1) * 0x85EBCA6Bu; }
    for (; i < to; i++) a = (a ^ t[i]) * 0x01000193u;
  };
  run(0, p0); run(p1, n);
  return (uint64_t(a) << 32) ^ b ^ n;
}
// (Claude v121) the replay key: a strong 128-bit hash of the tail without the projection (the draw key is compared as before).
// Computed on the FIFO worker once the frame is recorded (computeReplayKey), so a hit costs the render core two word compares instead of
// ~720 bytes plus the draw state's vectors (console v120: tail 8 + drawkey 6 us of a ~72 us hit).
inline void replayTailKey(const uint8_t* t, size_t n, uint64_t& x, uint64_t& y) {
  // four independent 32-bit lanes (ARM11: 32-bit multiplies are single instructions, 64-bit ones are not)
  const size_t p0 = offsetof(GxState, proj) - offsetof(GxState, kcolor), p1 = p0 + sizeof(GxProjection);
  uint32_t a = 0x811C9DC5u ^ uint32_t(n), b = 0x9E3779B9u, c = 0x85EBCA6Bu, d = 0xC2B2AE35u;
  auto rotl = [](uint32_t v, int r) { return (v << r) | (v >> (32 - r)); };
  auto run = [&](size_t from, size_t to) {
    for (size_t i = from; i + 4 <= to; i += 4) {
      uint32_t w; std::memcpy(&w, t + i, 4);
      a = rotl((a ^ w) * 0x01000193u, 13);
      b = (b + w) * 0x85EBCA6Bu; b ^= b >> 15;
      c = rotl(c ^ rotl(w, 7), 11) * 0xCC9E2D51u;
      d = (d + (w ^ 0x5BD1E995u)) * 0x1B873593u; d ^= d >> 13;
    }
  };
  run(0, p0); run(p1, n & ~size_t(3));
  for (size_t i = n & ~size_t(3); i < n; i++) a = (a ^ t[i]) * 0x01000193u;
  x = (uint64_t(a) << 32) | b; y = (uint64_t(c) << 32) | d;
}
}  // namespace
void computeReplayKey(RecordedEvent3ds& e) {
  e.rk[0] = e.rk[1] = 0;
  if (e.kind != RecordedEvent3ds::Draw || !e.geo) return;
  uint64_t tx, ty;
  replayTailKey(replayTailBegin(e.state()), replayTailBytes(e.state()), tx, ty);
  e.rk[0] = tx; e.rk[1] = ty;   // the draw key is still compared on the render core (hashing its vectors cost the worker ~11 us a draw)
  if (!e.rk[0] && !e.rk[1]) e.rk[0] = 1;
}
namespace {
constexpr size_t kReplayWindow = 6;      // previous-frame entries looked at past the cursor (draws added / removed since)
}
uint64_t g_replayProf[16] = {};   // (Claude v119 diag) replay hit cost, ticks: 0 lookup 1 remap 2 textures 3 repack 4 backend draw 5 hits counted; backend parts 6..11 (backend3ds.cpp)
bool g_replayProfOn = false;
uint64_t g_replayWhy[4] = {};   // (Claude v117 diag) misses: no entry near the cursor, tail bytes, draw state, 2D uniforms
bool Renderer3ds::replayTry(Ev& use, FrameReport& R, DrawReport& D, const ReplayGroupKey* group, Backend3ds::RecordedItem* prepared,std::shared_ptr<ReplayEntry>* resolved) {
  const bool pf = g_replayProfOn; uint64_t pt = pf ? svcGetSystemTick() : 0;
  auto pm = [&](int k) { if (pf) { const uint64_t t = svcGetSystemTick(); g_replayProf[k] += t - pt; pt = t; } };
  const uint64_t id = use.geo->id;
  auto range = replayPrev_.equal_range(id);
  pm(12);
  if (range.first == range.second) { R.replayMisses++; g_replayWhy[0]++; return false; }
  const size_t tb = replayTailBytes(use.state());
  const uint8_t* tail = replayTailBegin(use.state());
  // the projection may differ (camera field of view): it only reaches the frame mapping's proj matrix and the uniforms (checked below)
  const size_t pj0 = offsetof(GxState, proj) - offsetof(GxState, kcolor), pj1 = pj0 + sizeof(GxProjection);
  ReplayEntry* e = nullptr; decltype(range.first) at = range.second; int why = 1; bool projDiffers = false;
  // several entries of the same geometry (instances): compare a hash of the tail (projection excluded) first, the bytes only on a match
  const bool keyed = !group && (use.rk[0] | use.rk[1]) != 0;
  const bool multi = std::next(range.first) != range.second;
  const uint32_t gcount = group ? uint32_t(group->count) : 0;
  auto sameGroup = [&](const ReplayEntry& c) {   // (Claude v122) the same members (geometry) with the same slot maps and palette size: the same packed geometry
    if (c.groupCount != gcount) return false;
    if (!gcount) return true;
    if (c.groupPalette != group->palette) return false;
    for (size_t m = 0; m < gcount; m++) if (c.groupIds[m] != group->ids[m] || c.groupSlots[m] != group->slots[m]) return false;
    return true;
  };
  const uint64_t th = multi && !keyed ? replayTailHash(tail, tb) : 0;
  for (auto it = range.first; it != range.second; ++it) {
    ReplayEntry* c = it->second.get();
    if (!sameGroup(*c)) continue;
    if (keyed) {   // (Claude v121) key: tail without projection + draw key; the projection itself is compared (it may differ: remap below)
      if (c->rk[0] != use.rk[0] || c->rk[1] != use.rk[1] || c->tail.size() != tb) continue;
      const bool pd = !replayEq(c->tail.data() + pj0, tail + pj0, pj1 - pj0);
      if (pd && use.state().proj.ortho) continue;
      pm(13);
      if (!sameDrawKeyIgnoringPoseCount(c->draw, use.state().draw) || c->draw.numPosMtx != use.state().draw.numPosMtx || c->draw.vtx.numPosMtx != use.state().draw.vtx.numPosMtx) { why = 2; continue; }
      e = c; at = it; projDiffers = pd; pm(14); break;
    }
    if (c->tail.size() != tb || (multi && c->tailHash != th)) continue;
    const uint8_t* a = c->tail.data();
    if (!replayEq(a, tail, pj0) || !replayEq(a + pj1, tail + pj1, tb - pj1)) {
      static uint32_t n = 0;
      if ((++n % 211) == 1 && errLog_) {
        size_t f = 0; while (f < tb && a[f] == tail[f]) f++; size_t l = tb; while (l > f && a[l-1] == tail[l-1]) l--;
        errLog_("replay tail differs at bytes " + std::to_string(f) + ".." + std::to_string(l) + " (kcolor 0 tevReg 64 tex 128 viewport 544 scissor 568 proj 584" +
                " p " + std::to_string(pj0) + "+" + std::to_string(sizeof(GxProjection)) + ")");
      }
      continue;
    }
    // the projection may differ (camera field of view): it only reaches the frame mapping's proj matrix and the uniforms (checked below)
    const bool pd = !replayEq(a + pj0, tail + pj0, pj1 - pj0);
    if (pd && use.state().proj.ortho) continue;
    pm(13);
    if (!sameDrawKeyIgnoringPoseCount(c->draw, use.state().draw) || c->draw.numPosMtx != use.state().draw.numPosMtx || c->draw.vtx.numPosMtx != use.state().draw.vtx.numPosMtx) { why = 2; continue; }
    e = c; at = it; projDiffers = pd; pm(14); break;
  }
  if (!e) { R.replayMisses++; g_replayWhy[why]++; return false; }
  pm(0);
  // identity of the shared uniform snapshot; a state with neither carries its uniforms inline (GxState::uniforms): never treated as the same
  const bool sameUniforms = (use.state().uniformBlock || use.state().uniformSections) && e->uniformBlock == use.state().uniformBlock && e->uniformSections == use.state().uniformSections;
  const bool remap = projDiffers || (!sameUniforms && use.state().proj.ortho);   // a 2D draw's frame mapping reads its position matrices (widescreenXScale)
  // resident geometry: still the same buffers (mid-frame linear pressure or slot reuse may have freed them)
  // (Claude v122) the entry's resident geometry by pointer while no resident entry has been erased or replaced since it was found (residentGen_)
  ResidentEntry* res = e->residentGen == residentGen_ ? e->residentPtr : nullptr;
  if (!res) {
    auto rit = resident_.find(e->residentHash);
    if (rit == resident_.end() || rit->second->mem != e->residentMem || !(rit->second->key == e->residentKey)) { R.replayStale++; replayPrev_.erase(at); return false; }
    res = rit->second.get(); e->residentPtr = res; e->residentGen = residentGen_;
  }
  DrawPacket& P = e->packet;
  if (remap) {   // the frame mapping again: everything but P.frame.proj must come out the same
    const GxDraw& geom = use.geo->d;
    PicaFrame nf = mapFrame(use.state(), opt_.frame, P.pica().depthMap, widescreenXScale(use.state(), geom, opt_.frame));
    const PicaFrame& of = P.frame;
    if (nf.ok != of.ok || nf.mirrored != of.mirrored || nf.picaCull != of.picaCull || std::memcmp(nf.post, of.post, sizeof nf.post) != 0 ||
        std::memcmp(&nf.scissorLogical, &of.scissorLogical, sizeof(PixelRect)) != 0 || std::memcmp(&nf.scissorWindow, &of.scissorWindow, sizeof(PixelRect)) != 0 ||
        nf.depth.zBuffer != of.depth.zBuffer || std::memcmp(&nf.depth.scale, &of.depth.scale, sizeof(float)) != 0 || std::memcmp(&nf.depth.offset, &of.depth.offset, sizeof(float)) != 0 ||
        nf.depth.clear != of.depth.clear) { R.replayMisses++; g_replayWhy[projDiffers ? 1 : 3]++; return false; }
    std::memcpy(P.frame.proj, nf.proj, sizeof nf.proj);
    e->uniformBlock.reset(); e->uniformSections.reset();   // forces the uniforms below
    if (projDiffers) { std::memcpy(e->tail.data() + pj0, tail + pj0, pj1 - pj0); R.replayProj++; }
  }
  pm(1);
  // textures: the same lookups planUnits makes (content verification, LRU); the cached plan is valid only for the same entries
  for (int u = 0; u < P.pica().numUnits && u < 3; u++) {
    const SubmitUnit& SU = P.units[u];
    if (SU.scratch || SU.texmap < 0 || !SU.tex.valid) { R.replayStale++; replayPrev_.erase(at); return false; }
    if (cache_.validThisFrame(e->plan.units[u].entry)) continue;   // (Claude v122) checked by an earlier lookup of this frame: get() would return it
    TexMapOptions mo; mo.needAlpha = SU.needAlpha;
    TexLookup lk = cache_.get(SU.tex, *mem_, mo);
    if (!lk.entry || !lk.error.empty() || lk.mutableEntry() != e->plan.units[u].entry) { R.replayStale++; replayPrev_.erase(at); return false; }
  }
  // a plan that patched rendered-copy texScale rows owns a copy of that array: repacked rows would not reach it
  if ((!sameUniforms || remap) && !e->plan.ownedRows.empty()) { R.replayStale++; replayPrev_.erase(at); return false; }
  pm(2);
  res->lastFrame = frames_;   // (Claude v122) resident already: the seen set (it decides inserts of missing geometry) is not needed
  if (!sameUniforms || remap) {
    const bool poseOnly=uniformDelta_&&!remap&&use.state().uniformSections&&e->uniformSections&&
      ((use.state().uniformSections->shared&&use.state().uniformSections->shared==e->uniformSections->shared)||
       (use.state().uniformSections->pieces&&use.state().uniformSections->pieces==e->uniformSections->pieces)||
       (use.state().uniformSections->shared&&e->uniformSections->shared&&sameUsedNonPose(P.pica().vtx,P.pica().vs,*use.state().uniformSections->shared,*e->uniformSections->shared)));
    if(poseOnly){
      const auto view=use.state().uniformView();
      for(const ReplayEntry::RowMap& m:e->rowMap){
        if(m.array!=1&&m.array!=2)continue;
        const int rows=3*std::min(P.pica().vtx.numPosMtx,view.poseCount);
        if(int(m.first)+int(m.count)>rows){R.replayStale++;replayPrev_.erase(at);return false;}
        const auto* source=m.array==1?&view.pos[0][0][0]:&view.nrm[0][0][0];
        std::memcpy(&e->rows[m.at],source+size_t(m.first)*4,sizeof(PVec)*m.count);
      }
      ++R.uniformDeltaHits;
    }else{
    float scale[3][2]; std::memcpy(scale, use.state().uniformView().texScale, sizeof scale);
    for (int u = 0; u < P.pica().numUnits && u < 3; u++) { scale[u][0] = P.units[u].scaleS; scale[u][1] = P.units[u].scaleT; }
    if (!replayScratch_) replayScratch_.reset(new PackedUniforms());
    PackedUniforms& PU = *replayScratch_;
    packUniformsPacked(PU, P.pica().vtx, P.pica().vs, use.state().uniformView(), P.frame.proj, scale);
    for (const ReplayEntry::RowMap& m : e->rowMap) {
      const PackedUniformRows& A = PU.arrays[m.array];
      if (A.count < int(m.first) + int(m.count)) { R.replayStale++; replayPrev_.erase(at); return false; }   // the shader now reads more rows: rebuild
      std::memcpy(&e->rows[m.at], &A.rows[m.first], sizeof(PVec) * m.count);
    }
    }
    e->uniformBlock = use.state().uniformBlock; e->uniformSections = use.state().uniformSections;
  }
  pm(3);
  extern bool g_beProfOn; g_beProfOn = pf;
  D.packetOk = D.planOk = true;
  D.vertices = P.vertexCount; D.triangles = uint32_t(P.indexCount() / 3);
  const uint32_t rd0 = be_.recordedDraws();
  if(prepared){*prepared={&e->plan,&e->rec};D.drawn=true;}
  else D.drawn = recordOn_ ? be_.drawRecorded(e->plan, e->rec) : be_.draw(e->plan);
  R.replayRecorded += be_.recordedDraws() - rd0;
  g_beProfOn = false; pm(4); if (pf) g_replayProf[5]++;
  if (!D.drawn) D.why = be_.lastError();
  R.replayHits++;
  if(directEvents_&&!group)learnDirect(use,at->second);
  if(resolved)*resolved=at->second;
  e->availableFrame=~0ull;e->drawnFrame=frames_;
  replayCur_.insert(replayPrev_.extract(at));   // the node moves: no allocation per hit
  return true;
}
bool Renderer3ds::replayInsert(const GxState& st, uint64_t geoId, const uint64_t* rk, const ReplayGroupKey* group,std::shared_ptr<ReplayEntry>* inserted) {
  if (replayBytes_ >= replayBudget_.load(std::memory_order_relaxed)) return false;
  auto rit = resident_.find(residentLookup_.hash);
  if (rit == resident_.end() || rit->second->mem == nullptr) return false;
  auto e = std::make_shared<ReplayEntry>();
  e->geoId = geoId;
  e->draw = st.draw;
  e->tail.assign(replayTailBegin(st), replayTailBegin(st) + replayTailBytes(st));
  e->tailHash = replayTailHash(e->tail.data(), e->tail.size());
  if (rk && !group) { e->rk[0] = rk[0]; e->rk[1] = rk[1]; }
  if (group && group->count > 1) {   // (Claude v122)
    e->groupCount = uint32_t(group->count); e->groupPalette = uint32_t(group->palette);
    e->groupIds.assign(group->ids, group->ids + group->count); e->groupSlots.assign(group->slots, group->slots + group->count);
  }
  e->uniformBlock = st.uniformBlock; e->uniformSections = st.uniformSections;
  {
    std::vector<uint8_t> keepV; std::vector<uint16_t> keepI; keepV.swap(packet_.vertices); keepI.swap(packet_.indices);
    e->packet = packet_;
    keepV.swap(packet_.vertices); keepI.swap(packet_.indices);
  }
  // packet_'s rows only while the plan is built and compacted (below); the entry keeps no PackedUniforms
  e->packet.notes.clear();
  buildPlanInto(e->plan, e->packet, shaders_, cache_, *mem_, nullptr, false, nullptr);
  if (!e->plan.ok) return false;
  e->plan.linearGeometry = true;
  e->residentHash = residentLookup_.hash; e->residentMem = rit->second->mem; e->residentKey = rit->second->key;
  e->residentLease=rit->second;
  e->residentPtr = rit->second.get(); e->residentGen = residentGen_;
  {   // compact the rows the plan reads out of the packed arrays; uniforms outside them (plan.ownedRows) stay as they are
    const PackedUniforms& PU = *e->packet.packedUniforms;
    size_t n = 0;
    for (size_t i = 0; i < e->plan.uniforms.size(); i++) {
      const C3dUniform& cu = e->plan.uniforms[i];
      for (int k = 0; k < 8; k++) {
        const PVec* a = PU.arrays[k].rows;
        if (cu.rows >= a && cu.rows + cu.count <= a + 40) {
          if (i > 255) return false;
          e->rowMap.push_back({uint8_t(i), uint8_t(k), uint16_t(cu.rows - a), uint16_t(cu.count), uint16_t(n)}); n += size_t(cu.count); break;
        }
      }
    }
    e->rows.reset(new PVec[n ? n : 1]);
    for (const ReplayEntry::RowMap& m : e->rowMap) {
      std::memcpy(&e->rows[m.at], e->plan.uniforms[m.uniform].rows, sizeof(PVec) * m.count);
      e->plan.uniforms[m.uniform].rows = &e->rows[m.at];
    }
    if((modelPrograms_||directEvents_)&&e->plan.ownedRows.empty()&&!st.proj.ortho){
      auto recipe=std::make_shared<UniformRecipe>();std::vector<UniformRecipe::Range> ranges;
      for(const auto& m:e->rowMap)ranges.push_back({m.array,m.first,m.count,m.at});
      if(recipe->compile(e->packet.pica().vtx,e->packet.pica().vs,ranges)){
        for(const auto& old:uniformRecipes_)if(*old==*recipe){e->recipe=old;break;}
        if(!e->recipe&&uniformRecipes_.size()<128){uniformRecipes_.push_back(recipe);e->recipe=recipe;}
      }
    }
    e->packet.packedUniforms.reset();
    size_t b = sizeof(ReplayEntry) + 32 + e->tail.capacity() + n * sizeof(PVec) + e->rowMap.capacity() * sizeof(ReplayEntry::RowMap) +
               e->groupIds.capacity() * 8 + e->groupSlots.capacity() * 10 +
               e->plan.uniforms.capacity() * sizeof(C3dUniform) + e->plan.ownedRows.capacity() * sizeof(PVec);
    if (recordOn_) { be_.recordDraw(e->plan, e->rec); b += e->rec.bytes(); }   // (Claude v119) right after drawOne's draw() of the same plan
    e->bytes = b; e->live = &replayBytes_; replayBytes_ += b;
  }
  e->drawnFrame=frames_;
  if(modelPrograms_&&e->groupCount>1&&e->recipe&&e->rec.ok){
    if(!modelGroupPrograms_.count(geoId)&&modelGroupPrograms_.size()>=512)modelGroupPrograms_.erase(modelGroupPrograms_.begin());
    auto& group=modelGroupPrograms_[geoId];group.entries[group.next++&3]=e;
  }
  if(inserted)*inserted=e;
  replayCur_.emplace(geoId, std::move(e));
  return true;
}

#include "model_program3ds.inc"
#include "direct_events3ds.inc"
#include "direct_groups3ds.inc"

// (Claude v117) before matrix grouping and the compile lookup: an event that equals a single draw of the previous frame is submitted from its plan
bool Renderer3ds::replayEvent(Ev& x, int index, FrameReport& R,size_t* consumed) {
  if (!replayOn_ || !x.geo || drawRouted_ || edit_ || tweak_ || opt_.directPack || !useDrawCache_ || !residentOn_ || reuseOn_ || opt_.packetNotes) return false;
  if (replayPrev_.empty()) { R.replayMisses++; g_replayWhy[0]++; return false; }
  if(modelPrograms_&&consumed&&executeModelProgram(index,R,*consumed))return true;
  const bool timed=drawTimingEnabled_;const u64 t0=timed?svcGetSystemTick():0;
  std::array<std::shared_ptr<ReplayEntry>,8> resolved{};
  std::array<Backend3ds::RecordedItem,8> prepared{};std::array<DrawReport,8> reports{};size_t count=0;
  const bool block=(commandBlocks_||modelPrograms_)&&recordOn_&&consumed;
  const size_t limit=block?std::min(size_t(8),evs_.size()):1;
  for(size_t k=0;k<limit;++k){
    Ev& use=k?evs_[k]:x;if(use.kind!=Ev::Draw||!use.geo||use.state().proj.ortho!=x.state().proj.ortho)break;
    use.state().draw.texmapScaled|=cache_.scaledRenderedTexmaps(use.state().tex);
    reports[k].index=index+int(k);
    const auto misses=R.replayMisses,stale=R.replayStale;uint64_t why[4];std::memcpy(why,g_replayWhy,sizeof why);
    if(!replayTry(use,R,reports[k],nullptr,block?&prepared[k]:nullptr,modelPrograms_?&resolved[k]:nullptr)){
      if(!k)return false;
      // Lookahead must not count a second miss when the next event executes.
      R.replayMisses=misses;R.replayStale=stale;std::memcpy(g_replayWhy,why,sizeof why);break;
    }
    ++count;
  }
  if(modelPrograms_)learnModelProgram(resolved.data(),count);
  if(block){
    const uint32_t before=be_.recordedDraws();
    for(size_t at=0;at<count;){
      size_t span=1;int done=-1;
      const auto& first=*prepared[at].record;
      while(at+span<count){const auto& next=*prepared[at+span].record;if(next.mat!=first.mat||next.program!=first.program)break;++span;}
      if(span>=2)done=be_.drawRecordedBlock(prepared.data()+at,span);
      if(done<0){reports[at].drawn=be_.drawRecorded(*prepared[at].plan,*prepared[at].record);++at;}
      else{for(size_t j=0;j<span;++j)reports[at+j].drawn=j<size_t(done);at+=span;}
    }
    R.replayRecorded+=be_.recordedDraws()-before;
  }
  planPrevOwner_.reset();const u64 t3=timed?svcGetSystemTick():0;
  for(size_t k=0;k<count;++k){auto& D=reports[k];++R.draws;if(D.drawn)++R.drawn;else{++R.refused;D.why=be_.lastError();}
    D.drawMs=k?0:msOf(t3-t0);R.drawMs+=D.drawMs;
    if(drawLog_){static const std::vector<std::string> none;D.packetNotes=&none;D.planNotes=&none;drawLog_(D);}}
  if(consumed)*consumed=count;
  return true;
}

// (Claude v122) a matrix group (formed this frame: x.state() holds the group's uniform block and variant key) that equals a group of the previous frame:
// the same members (geometry ids), slot maps, palette size, member 0 state (tail) and variant key. Its uniforms are packed again from x.state().
bool Renderer3ds::replayGroupEvent(Ev& x, const matrixbatch::Group& group, int index, FrameReport& R,const Options& options) {
  if (!replayOn_ || !replayGroups_ || !x.geo || drawRouted_ || edit_ || tweak_ || opt_.directPack || !useDrawCache_ || !residentOn_ || reuseOn_ || opt_.packetNotes) return false;
  if (replayPrev_.empty() || group.count < 2 || group.count > evs_.size()) return false;
  uint64_t ids[matrixbatch::Group::MaxMembers];
  for (size_t m = 0; m < group.count; m++) { if (!evs_[m].geo) return false; ids[m] = evs_[m].geo->id; }
  ReplayGroupKey gk; gk.ids = ids; gk.slots = group.slots.data(); gk.count = group.count; gk.palette = group.paletteSize;
  const bool timed = drawTimingEnabled_;
  const u64 t0 = timed ? svcGetSystemTick() : 0;
  DrawReport D; D.index = index;
  const uint32_t misses = R.replayMisses;
  std::shared_ptr<ReplayEntry> resolved;
  if (!replayTry(x, R, D, &gk,nullptr,&resolved)) { R.replayMisses = misses; R.replayGroupMisses++; return false; }   // (member 0 already counted its miss)
  R.replayGroupHits++;
  if(directGroups_)learnDirectGroup(resolved,options);
  planPrevOwner_.reset();
  const u64 t3 = timed ? svcGetSystemTick() : 0;
  D.drawMs = msOf(t3 - t0);
  R.draws++;
  if (D.drawn) R.drawn++; else R.refused++;
  R.drawMs += D.drawMs;
  if (drawLog_) { static const std::vector<std::string> none; D.packetNotes = &none; D.planNotes = &none; drawLog_(D); }
  return true;
}

void Renderer3ds::drawOne(Ev& x, const Options& opt, int index, FrameReport& R, const matrixbatch::Group* group,const DrawPacket* prepared,std::shared_ptr<const PicaDraw> known,std::shared_ptr<PackedUniforms> rows) {
  DrawReport D;
  D.index = index;
  // Timing every draw needs four kernel calls; the port logs only every60
  // frames. Keep detailed timing on those frames, with an opt-in full mode.
  const bool timed = drawTimingEnabled_;
  const u64 t0 = timed ? svcGetSystemTick() : 0;
  const FrameConfig& fc = drawCfg_ ? *drawCfg_ : opt_.frame;   // (Claude v80) the route's frame config for a routed draw
  std::unique_ptr<Ev> edited;
  if (edit_) { edited.reset(new Ev(x)); if (edited->geo) { edited->d = edited->geo->d; edited->geo.reset(); } edit_(edited->state(), edited->d, index); }
  Ev& use = edited ? *edited : x;
  const GxDraw& geom = use.geo ? use.geo->d : use.d;
  // Only the render owner consumes this queued event. Account for GPU-copy
  // padding before selecting its shader; the packet binding alone cannot know
  // the copy layout. buildPlan then supplies the actual scale uniform.
  use.state().draw.texmapScaled |= cache_.scaledRenderedTexmaps(use.state().tex);
  replayNoInsert_ = false;
  const bool replayEligible = !rows && replayOn_ && !edited && (!group || replayGroups_) && !prepared && use.geo && !drawRouted_ && !edit_ && !tweak_ && !opt_.directPack &&
                              useDrawCache_ && residentOn_ && !reuseOn_ && !opt_.packetNotes;
  R3T("build", index);
  // (Claude v73) cross-frame draw reuse (drawreuse3ds.h): the same compiled draw, state and geometry as a draw of an earlier frame
  reuse::Entry* hit=nullptr;std::shared_ptr<const PicaDraw> compiled=(edited||!useDrawCache_)?nullptr:std::move(known);bool reuseTry=false;uint64_t reuseLookup=0;
  // (Claude v83) translate-once: when every member is retained geometry, the key names it by id (no hashing); with retained geometry on, only such draws
  // are reused (the others changed this frame, hashing them would be wasted)
  uint64_t ids[matrixbatch::Group::MaxMembers];bool idKey=!edited && use.geo;
  if(idKey){ if(group){for(size_t m=0;m<group->count&&idKey;m++){if(m>=evs_.size()||!evs_[m].geo)idKey=false;else ids[m]=evs_[m].geo->id;}} else ids[0]=use.geo->id; }
  if(!rows && reuseOn_ && !drawRouted_ && !prepared && (retainOn_?idKey:!use.geo) && !edit_ && !tweak_ && !opt_.directPack && useDrawCache_ && !opt_.packetNotes) {   // (Claude v80) the reuse key has no frame config: not for routed draws
    if(!compiled)compiled=drawCache_.get(use.state().draw,opt);
    if(compiled && compiled->overall!=Fidelity::Unsupported && !compiled->pass2 && compiled->vs.ok) {
      reuseTry=true;
      if(idKey)reuse::makeKeyIds(reuseKey_,compiled.get(),use.state(),geom,group,ids);else reuse::makeKey(reuseKey_,compiled.get(),use.state(),geom,group);
      reuseLookup=reuseKey_.lookup();
      auto it=reuse_.find(reuseLookup);
      if(it!=reuse_.end() && it->second->key==reuseKey_) {
        reuse::Entry& e=*it->second;DrawPacket& P=e.packet;
        P.frame=mapFrame(use.state(),opt_.frame,P.pica().depthMap,widescreenXScale(use.state(),geom,opt_.frame));   // reads the projection (and 2D position) matrices
        if(P.frame.ok && !P.frame.scissorWindow.empty()) {
          float scale[3][2];std::memcpy(scale,use.state().uniformView().texScale,sizeof scale);
          for(int u=0;u<P.pica().numUnits;u++){scale[u][0]=P.units[u].scaleS;scale[u][1]=P.units[u].scaleT;}
          packUniformsPacked(*P.packedUniforms,P.pica().vtx,P.pica().vs,use.state().uniformView(),P.frame.proj,scale);
          e.lastFrame=frames_;hit=&e;R.reuseHits++;
        }
      }
      if(!hit){R.reuseMisses++;reuseSeenCur_.insert(reuseLookup);}
    }
  }
  const size_t geometryMark=be_.geometryMark();
  PacketGeometryOutput output{&be_,[](void* b,size_t bytes,size_t count,uint8_t*& v,uint16_t*& i){
    return static_cast<Backend3ds*>(b)->reserveGeometry(bytes,count,v,i);
  }};
  // (Claude v86) resident geometry: every member retained; its packed buffers may already be in linear memory (no packing, no arena copy)
  const bool residentTry=residentOn_ && idKey && !prepared && !hit && !reuseTry && !edit_ && !tweak_ && !opt_.directPack && useDrawCache_;
  PacketGeometryOutput residentOut;
  if(residentTry){residentLookup_.ids=ids;residentLookup_.idCount=group?group->count:1;residentLookup_.context=this;residentLookup_.find=&Renderer3ds::residentFind;residentLookup_.hit=false;residentLookup_.hash=0;residentOut.resident=&residentLookup_;}
  if(!prepared && !hit)buildDrawInto(packet_, use.state(), geom, opt, fc, useDrawCache_ ? &drawCache_ : nullptr, opt_.packetNotes,
                (use.geo && !group && !reuseTry && !residentTry) ? &packCache_ : nullptr, (use.geo && !group && !reuseTry && !residentTry) ? use.geo->id : 0,
                opt_.directPack ? &output : residentTry ? &residentOut : nullptr,group,compiled,rows);   // (Claude v83) a matrix group packs its members (no pack cache)
  const bool residentHit=residentTry && residentLookup_.hit && packet_.externalVertices;
  const DrawPacket& pk = prepared?*prepared:hit?hit->packet:packet_;
  if(prepared&&opt_.packetWorkerVerify){
    DrawPacket reference;Options check=opt;check.fogCompileCache=nullptr;
    buildDrawInto(reference,use.state(),geom,check,fc,nullptr,opt_.packetNotes,nullptr,0,nullptr,nullptr,packetOracleProgram(pk.pica()));
    ++R.packetChecks;
    if(!samePreparedPacket(pk,reference)){++R.packetMismatches;if(errLog_)errLog_("packet-worker packet differs at draw "+std::to_string(index)+" field line "+std::to_string(packetDiffLine));}
  }
  R3T("plan", index);
  const u64 t1 = timed ? svcGetSystemTick() : 0;
  C3dPlan freshPlan;
  C3dPlan& plan=planReuse_?planScratch_:freshPlan;
  C3dMultiPlan multi;
  std::vector<std::string> planNotes;
  D.twoPass = pk.ok && pk.second;
  if (D.twoPass) {
    if (drawRouted_) {                   // (Claude v80) the scratch pass is made for the top target; no UI map draw is two-pass
      multi.why = "two-pass draw on the bottom-screen route";
      R.routeRefused++;
    } else if (!ensureScratch()) {
      multi.why = "no scratch target (VRAM exhausted?)";
    } else {
      multi = buildMultiPlan(pk, shaders_, cache_, *mem_, &scratchView_,opt_.planNotes);
      for (const std::string& n : multi.first.notes) planNotes.push_back(n);
      for (const std::string& n : multi.second.notes) planNotes.push_back("second pass: " + n);
      if (tweak_) { tweak_(multi.first, index); c3dFinalize(multi.first); tweak_(multi.second, index); c3dFinalize(multi.second); }
    }
  } else {
    // (Claude v87) plan delta: the previous event of this frame built planScratch_ from the same compiled draw and this event changed only state the
    // delta steps re-read (c3dplan.h buildPlanDelta)
    constexpr uint32_t kDeltaDirty=kStateDirtyTevReg|kStateDirtyView|kStateDirtyXfMtx|kStateDirtyLight;
    const bool deltaTry=planDelta_ && planReuse_ && !edited && !tweak_ && !prepared && !group && curEvAt_!=~size_t(0) && curEvAt_==planPrevAt_+1 &&
      drawRouted_==planPrevRouted_ && (x.dirtyIn&~kDeltaDirty)==0 && planPrevOwner_ && planPrevOwner_.get()==&pk.pica();
    bool delta=false;
    if(deltaTry){delta=buildPlanDelta(plan,pk,opt_.planNotes);if(delta)R.planDeltas++;else R.planDeltaFails++;}
    if(!delta){
      if(planReuse_)buildPlanInto(plan,pk,shaders_,cache_,*mem_,nullptr,opt_.planNotes,planTemplates_.get());
      else plan = buildPlan(pk, shaders_, cache_, *mem_,nullptr,opt_.planNotes,planTemplates_.get());
    }else if(planDeltaVerify_){
      C3dPlan ref;buildPlanInto(ref,pk,shaders_,cache_,*mem_,nullptr,opt_.planNotes,nullptr);
      ++R.planDeltaChecks;
      const char* bad=samePlanDelta(plan,ref);
      if(bad){++R.planDeltaMismatches;if(errLog_)errLog_(std::string("plan-delta differs at draw ")+std::to_string(index)+": "+bad);}
    }
    planNotes = plan.notes;
    if (tweak_ && plan.ok) { tweak_(plan, index); c3dFinalize(plan); }
    if (hit || residentHit) plan.linearGeometry = true;
    if(planDelta_ && planReuse_ && plan.ok && !tweak_ && !edited && !prepared && !group){planPrevAt_=curEvAt_;planPrevRouted_=drawRouted_;planPrevOwner_=pk.picaOwner().lock();}
    else planPrevOwner_.reset();
  }
  if(D.twoPass)planPrevOwner_.reset();
  const u64 t2 = timed ? svcGetSystemTick() : 0;
  D.packetOk = pk.ok;
  D.planOk = pk.ok && (D.twoPass ? multi.ok : plan.ok);
  D.vertices = pk.vertexCount;
  D.triangles = uint32_t(pk.indexCount() / 3);
  D.why = pk.ok ? (D.twoPass ? multi.why : plan.why) : std::string("packet refused");
  R3T("draw", index);
  if (D.planOk) {
    D.drawn = D.twoPass ? be_.drawTwoPass(multi, scratch_, target_) : be_.draw(plan);
    if (!D.drawn) D.why = be_.lastError();
    else if (D.twoPass) R.twoPass++;
  }
  if(!prepared && !hit && !residentHit && packet_.externalVertices && !D.drawn)be_.rewindGeometry(geometryMark);
  if(residentTry){
    if(residentHit)R.residentHits++;
    else if(residentLookup_.hash){
      R.residentMisses++;
      // seen (missed) in the previous frame too and drawn now: keep its packed buffers in linear memory
      if(D.drawn && !D.twoPass && packet_.ok && !packet_.externalVertices && residentSeenPrev_.count(residentLookup_.hash)) {
        const size_t vb=packet_.vertexBytes(),ib=packet_.indexCount()*sizeof(uint16_t),at=(vb+15)&~size_t(15),bytes=at+ib;
        auto it=resident_.find(residentLookup_.hash);
        const bool slotFree=it==resident_.end() || it->second->lastFrame+4<=frames_;
        if(!slotFree || residentBytes_+bytes>residentBudget_ || !vb || !ib) R.residentFull++;
        else if(void* m=linearAlloc(bytes)) {
          std::memcpy(m,packet_.vertexData(),vb);std::memcpy(static_cast<uint8_t*>(m)+at,packet_.indexData(),ib);
          GSPGPU_FlushDataCache(m,u32(bytes));
          auto e=std::make_shared<ResidentEntry>();
          e->key=residentLookup_.key;e->mem=m;e->bytes=bytes;e->lastFrame=frames_;
          e->hit.vertices=static_cast<uint8_t*>(m);e->hit.indices=reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(m)+at);
          e->hit.missingNrm=packet_.packMissingNrm;e->hit.missingClr=packet_.packMissingClr;e->hit.indicesChecked=packet_.indicesChecked;
          if(it!=resident_.end()){residentBytes_-=it->second->bytes;linearFree(it->second->mem);it->second->mem=nullptr;it->second=std::move(e);residentGen_++;}
          else resident_.emplace(residentLookup_.hash,std::move(e));
          residentBytes_+=bytes;R.residentInserts++;
        } else R.residentFull++;
      }
      residentSeenCur_.insert(residentLookup_.hash);
    }
  }
  if (replayEligible && residentHit && D.drawn && !D.twoPass && !hit && packet_.ok && !packet_.second && packet_.usesPackedUniforms && packet_.packedUniforms &&
      packet_.externalVertices && !packet_.packedFrom && !packet_.packKey && !replayNoInsert_ && (!group || idKey)) {
    ReplayGroupKey gk; if (group) { gk.ids = ids; gk.slots = group->slots.data(); gk.count = group->count; gk.palette = group->paletteSize; }   // (Claude v122)
    std::shared_ptr<ReplayEntry> inserted;
    if (replayInsert(use.state(), use.geo->id, use.rk, group && group->count > 1 ? &gk : nullptr,&inserted)) {
      R.replayInserts++;
      if(directEvents_&&!group)learnDirect(use,inserted);
      if(directGroups_&&group)learnDirectGroup(inserted,opt);
    }
  }
  if (!prepared && !hit && (packet_.packedFrom || packet_.packKey)) releasePacked(packet_, packCache_, D.planOk && !D.twoPass);
  // (Claude v73) a draw seen in the previous frame and drawn now: keep its packet, with its geometry in linear memory
  if(reuseTry && !hit && D.drawn && !D.twoPass && packet_.ok && !packet_.second && packet_.usesPackedUniforms && packet_.packedUniforms &&
     !packet_.externalVertices && !packet_.packedFrom && !packet_.packKey && reuseSeenPrev_.count(reuseLookup)) {
    const size_t vb=packet_.vertexBytes(),ib=packet_.indexCount()*sizeof(uint16_t),at=(vb+15)&~size_t(15),bytes=at+ib;
    auto it=reuse_.find(reuseLookup);
    const bool slotFree=it==reuse_.end() || it->second->lastFrame+4<=frames_;
    if(!slotFree || reuseBytes_+bytes>reuseBudget_) R.reuseFull++;
    else if(void* m=linearAlloc(bytes)) {
      std::memcpy(m,packet_.vertexData(),vb);std::memcpy(static_cast<uint8_t*>(m)+at,packet_.indexData(),ib);
      GSPGPU_FlushDataCache(m,u32(bytes));
      auto e=std::make_unique<reuse::Entry>();
      std::vector<uint8_t> keepV;std::vector<uint16_t> keepI;keepV.swap(packet_.vertices);keepI.swap(packet_.indices);
      e->packet=packet_;
      keepV.swap(packet_.vertices);keepI.swap(packet_.indices);
      e->packet.packedUniforms=std::make_shared<PackedUniforms>(*packet_.packedUniforms);
      e->packet.externalVertices=static_cast<uint8_t*>(m);e->packet.externalIndices=reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(m)+at);
      e->packet.externalIndexCount=packet_.indexCount();
      e->key=reuseKey_;e->mem=m;e->bytes=bytes;e->lastFrame=frames_;
      if(it!=reuse_.end()){reuseBytes_-=it->second->bytes;linearFree(it->second->mem);it->second=std::move(e);}
      else reuse_.emplace(reuseLookup,std::move(e));
      reuseBytes_+=bytes;R.reuseInserts++;
    } else R.reuseFull++;
  }
  const u64 t3 = timed ? svcGetSystemTick() : 0;
  D.buildMs = msOf(t1 - t0); D.planMs = msOf(t2 - t1); D.drawMs = msOf(t3 - t2);
  R.draws++;
  if (D.drawn) R.drawn++; else R.refused++;
  R.buildMs += D.buildMs; R.planMs += D.planMs; R.drawMs += D.drawMs;
  if (drawLog_) {
    D.packetNotes = &pk.notes;
    D.planNotes = &planNotes;
    drawLog_(D);
  }
}

// ---------------------------------------------------------------- (Claude v106) synthesized warp textures
// The GameCube sea moves its texture with an indirect warp: every pixel's texture coordinate is offset by a scrolling 64x64 I8 map (per-pixel
// dependent reads the PICA cannot do; the sea mesh's vertices are 800 units apart against a 2000-unit warp period, so a per-vertex warp would alias).
// Instead the game registers the warp here and, each frame, issues a no-clear GXCopyTex to the registered address with the scroll in the copy's
// source x: the renderer draws a finely tessellated mesh whose texture coordinates carry the warp, sampling the real texture, into a mipmapped
// render texture (one pass per level, the source's own mip chain doing the filtering), and the game binds that texture with the indirect stages off.
extern "C" {
struct N3dsTexWarp {
  uint32_t size;
  uint32_t dst; uint16_t outW, outH; uint8_t levels, pad0[3];
  uint32_t dstGxBytes;
  uint32_t src; uint16_t srcW, srcH; uint8_t srcFmt, srcLevels, pad1[2];
  uint32_t ind; uint16_t indW, indH;
  float period, srcScale, srcOffS, srcOffT, indTexels, scrollDiv;
};
volatile int g_n3dsTexWarpFailed = 0;
}
namespace {
constexpr int kWarpMax = 4;
N3dsTexWarp g_warpReg[kWarpMax];
std::atomic<int> g_warpCount{0};
struct WarpRt { int lastParam = -100000; std::vector<float> ind; uint32_t indAddr = 0; };
WarpRt g_warpRt[kWarpMax];
// the source and map bytes, copied at registration (static archive data; the renderer's memory view only holds what the frame's draws read)
std::vector<uint8_t> g_warpSrc[kWarpMax], g_warpInd[kWarpMax];
struct WarpMem : GuestMem {
  uint32_t addr = 0; const std::vector<uint8_t>* bytes = nullptr;
  const uint8_t* read(uint32_t a, size_t n) const override {
    if (!bytes || a < addr || uint64_t(a) + n > uint64_t(addr) + bytes->size()) return nullptr;
    return bytes->data() + (a - addr);
  }
};
size_t gxI4Bytes(int w, int h, int levels) { size_t t = 0; for (int l = 0; l < levels; l++) t += size_t(std::max(8, w >> l)) * size_t(std::max(8, h >> l)) / 2; return t; }
}  // namespace
extern "C" int n3dsRegisterTexWarp(const N3dsTexWarp* w) {
  if (!w || w->size != sizeof(N3dsTexWarp)) return -1;
  const int n = g_warpCount.load(std::memory_order_acquire);
  for (int i = 0; i < n; i++) if (g_warpReg[i].dst == w->dst) return i;   // registered once per address
  if (n >= kWarpMax) return -1;
  if (w->srcFmt != 0 || !w->src || !w->ind) return -1;   // GX I4 source, I8 map
  const size_t sb = gxI4Bytes(w->srcW, w->srcH, std::max<int>(1, w->srcLevels)), ib = size_t(w->indW) * w->indH;
  g_warpSrc[n].assign(reinterpret_cast<const uint8_t*>(uintptr_t(w->src)), reinterpret_cast<const uint8_t*>(uintptr_t(w->src)) + sb);
  g_warpInd[n].assign(reinterpret_cast<const uint8_t*>(uintptr_t(w->ind)), reinterpret_cast<const uint8_t*>(uintptr_t(w->ind)) + ib);
  g_warpReg[n] = *w;
  g_warpCount.store(n + 1, std::memory_order_release);
  return n;
}
extern "C" int n3dsTexWarpFailed(void) { return g_n3dsTexWarpFailed; }

bool Renderer3ds::warpOne(const GxCopy& c) {
  const int n = g_warpCount.load(std::memory_order_acquire);
  int slot = -1;
  for (int i = 0; i < n; i++) if (g_warpReg[i].dst == c.dstAddr) { slot = i; break; }
  if (slot < 0) return false;
  const N3dsTexWarp& W = g_warpReg[slot];
  WarpRt& RT = g_warpRt[slot];
  auto failed = [&](const char* why) { if (!g_n3dsTexWarpFailed) ::n3dsLog("[warp] %08lx failed: %s %s\n", (unsigned long)W.dst, why, be_.lastError().c_str()); g_n3dsTexWarpFailed = 1; return true; };
  if (g_n3dsTexWarpFailed || !mem_) return true;
  const int param = c.srcX;
  TexEntry* e = const_cast<TexEntry*>(cache_.renderedAt(W.dst));
  const bool fresh = !e || !e->user || e->tex.levels != W.levels;
  if (!fresh) {   // the scroll moves 1/300 of a period per game frame: each texture is redrawn every other frame (the two alternate)
    int d = param - RT.lastParam; if (d < 0) d = -d;
    if (d == 0 || (d <= 2 && ((param + slot) & 1) != 0)) return true;
  }
  // the indirect map, decoded once (I8: 8x4 tiles)
  if (RT.indAddr != W.ind || RT.ind.size() != size_t(W.indW) * W.indH) {
    if (g_warpInd[slot].size() != size_t(W.indW) * W.indH) return failed("indirect texture not registered");
    const uint8_t* b = g_warpInd[slot].data();
    RT.ind.assign(size_t(W.indW) * W.indH, 0.0f);
    size_t i = 0;
    for (int by = 0; by < W.indH; by += 4) for (int bx = 0; bx < W.indW; bx += 8) for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
      const uint8_t v = b[i++];
      if (by + y < W.indH && bx + x < W.indW) RT.ind[size_t(by + y) * W.indW + size_t(bx + x)] = float(v);
    }
    RT.indAddr = W.ind;
  }
  // the source texture through the cache (uploaded by the first draw that uses it)
  GxTexBinding sb;
  sb.valid = true; sb.addr = W.src; sb.width = W.srcW; sb.height = W.srcH; sb.fmt = W.srcFmt; sb.wrapS = sb.wrapT = 1;
  sb.magLinear = sb.minLinear = true; sb.mipMode = W.srcLevels > 1 ? 2 : 0; sb.maxLod = float(W.srcLevels - 1);
  TexMapOptions mo; mo.needAlpha = true;
  WarpMem wm; wm.addr = W.src; wm.bytes = &g_warpSrc[slot];
  TexLookup lk = cache_.get(sb, wm, mo);
  if (!lk.entry || !lk.error.empty()) return failed(("source texture: " + lk.error).c_str());
  if (fresh) {
    RenderedTexture r;
    r.addr = W.dst; r.gxFmt = 0; r.gxW = W.outW; r.gxH = W.outH; r.guestBytes = W.dstGxBytes;
    r.pfmt = PTF_RGBA8; r.allocW = r.portW = W.outW; r.allocH = r.portH = W.outH;
    r.cost = size_t(W.outW) * W.outH * 4 * 4 / 3;
    e = cache_.addRendered(r);
    if (!e) return failed("addRendered");
    e->tex.levels = W.levels;
    if (!be_.allocRenderedMips(*e, W.levels)) {
      e->tex.fmt = PTF_RGBA4; e->cost /= 2;
      if (!be_.allocRenderedMips(*e, W.levels)) { cache_.invalidateRange(W.dst, W.dstGxBytes ? W.dstGxBytes : 1); return failed("allocation"); }
    }
  }
  // the warped source coordinate of output point p (GX texture space; PICA t runs the other way: translate.h flipTexT)
  const float scroll = W.scrollDiv != 0 ? float(param) / W.scrollDiv : 0.0f;
  const int iw = W.indW, ih = W.indH;
  auto indAt = [&](float s, float t) {   // bilinear, repeat, texel centres at +0.5
    float x = s * iw - 0.5f, y = t * ih - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float ax = x - fx, ay = y - fy;
    int x0 = int(fx) % iw; if (x0 < 0) x0 += iw;
    int y0 = int(fy) % ih; if (y0 < 0) y0 += ih;
    const int x1 = (x0 + 1) % iw, y1 = (y0 + 1) % ih;
    const float* m = RT.ind.data();
    const float a = m[y0 * iw + x0] + (m[y0 * iw + x1] - m[y0 * iw + x0]) * ax;
    const float b = m[y1 * iw + x0] + (m[y1 * iw + x1] - m[y1 * iw + x0]) * ax;
    return a + (b - a) * ay;
  };
  // one vertex grid at the finest tessellation (16 cells per warp period: the map is smooth, 4 map texels per cell, interpolated linearly
  // between), shared by every level; coarser levels index every k-th vertex (at least 8 target pixels per cell)
  const u64 t0 = svcGetSystemTick();
  const int cells0 = std::max(2, int(16.0f * W.period + 0.5f));
  static std::vector<float> verts; static std::vector<uint16_t> idx[8]; static int idxCells[8][2];
  verts.resize(size_t(cells0 + 1) * (cells0 + 1) * 5);
  float* v = verts.data();
  const float invW = 1.0f / float(W.srcW), invH = 1.0f / float(W.srcH);
  for (int j = 0; j <= cells0; j++) {
    const float pt = float(j) / cells0, ut = pt * W.period;
    for (int i = 0; i <= cells0; i++) {
      const float ps = float(i) / cells0, us = ps * W.period;
      const float d = W.indTexels * (indAt(us, ut + scroll) - 128.0f);
      v[0] = -1.0f + 2.0f * ps; v[1] = 1.0f - 2.0f * pt; v[2] = -0.5f;
      v[3] = W.srcScale * us + W.srcOffS + d * invW;
      v[4] = 1.0f - (W.srcScale * ut + W.srcOffT + d * invH);
      v += 5;
    }
  }
  static std::vector<uint8_t> vbytes;
  vbytes.resize(verts.size() * sizeof(float));
  std::memcpy(vbytes.data(), verts.data(), vbytes.size());
  for (int l = 0; l < W.levels && l < 8; l++) {
    const int size = std::min(W.outW, W.outH) >> l;
    int cells = std::max(2, std::min(cells0, size / 8));
    while (cells0 % cells) cells--;
    const int k = cells0 / cells;
    std::vector<uint16_t>& ix = idx[l];
    if (idxCells[l][0] != cells0 || idxCells[l][1] != cells) {
      ix.clear();
      for (int j = 0; j < cells; j++) for (int i = 0; i < cells; i++) {
        const uint16_t a = uint16_t((j * k) * (cells0 + 1) + i * k), b = uint16_t(a + k), c2 = uint16_t(a + k * (cells0 + 1)), d2 = uint16_t(c2 + k);
        const uint16_t q[6] = {a, b, d2, a, d2, c2};
        ix.insert(ix.end(), q, q + 6);
      }
      idxCells[l][0] = cells0; idxCells[l][1] = cells;
    }
    C3dPlan L;
    std::string why;
    if (!planTexturedMesh(L, shaders_, lk.mutableEntry(), W.srcLevels - 1, vbytes, ix, W.outW >> l, W.outH >> l, why)) return failed(why.c_str());
    if (!be_.drawIntoLevel(*e, l, L, target_)) return failed("draw");
  }
  {   // (diag) N3DS_WARP_LOG=1: mean synthesis time every 60 syntheses
    static int logOn = -1; static u64 acc = 0; static int cnt = 0;
    if (logOn < 0) { const char* ev = getenv("N3DS_WARP_LOG"); logOn = ev && *ev == '1'; }
    acc += svcGetSystemTick() - t0;
    if (logOn && ++cnt == 60) { ::n3dsLog("[warp] mean synthesis %.3f ms\n", double(acc) / 60.0 / (SYSCLOCK_ARM11 / 1000.0)); acc = 0; cnt = 0; }
  }
  RT.lastParam = param;
  static bool logged = false;
  if (!logged) { logged = true; ::n3dsLog("[warp] %08lx %dx%d levels %d from %08lx: first synthesis ok (fmt %d)\n", (unsigned long)W.dst, W.outW, W.outH, W.levels, (unsigned long)W.src, e->tex.fmt); }
  return true;
}

void Renderer3ds::copyOne(const GxCopy& c, const GxState& st, const EfbCopyOptions& co, int index, bool xfbClear, FrameReport& R) {
  if (!c.toXfb && !c.clear && warpOne(c)) return;
  CopyReport C;
  C.index = index;
  C.toXfbClear = xfbClear;
  C.copy = &c;
  const u64 t0 = svcGetSystemTick();
  EfbCopyPlan plan = planEfbCopy(c, st, opt_.frame, shaders_, &view_, co);
  const u64 t1 = svcGetSystemTick();
  C.why = plan.why;
  if (plan.ok) {
    C.done = be_.copy(plan, cache_, target_);
    if (!C.done) C.why = be_.lastError();
  }
  const u64 t2 = svcGetSystemTick();
  C.planMs = msOf(t1 - t0); C.drawMs = msOf(t2 - t1);
  C.plan = &plan;
  R.copies++;
  if (C.done) R.copiesDone++;
  R.planMs += C.planMs; R.drawMs += C.drawMs;
  if (copyLog_) copyLog_(C);
}

}  // namespace t2p
