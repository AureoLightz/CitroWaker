#pragma once
#ifdef N3DS_RECORD_FRAME_TYPES_HEADER
#include N3DS_RECORD_FRAME_TYPES_HEADER
#else
#include "renderer3ds.h"
#endif
#include "render_snapshot_3ds.h"
#ifndef N3DS_RECORD_FRAME_TYPES_HEADER
#include "state_templates.h"
#endif
#ifdef __3DS__
#include "typed_gx_api_3ds.h"
#endif
namespace t2p {
struct CpuFrame3ds : RecordedFrame3ds {
  RenderSnapshot3ds snapshot;
  uint64_t processTicks=0, prof[16]={};
  size_t compactSavedBytes=0;unsigned compactBatches=0;
  unsigned templateHits=0,templateMisses=0;
  size_t retainedBytes=0;   // (Claude v83) decoded geometry published to the retainer this frame
  uint64_t stagingWaitTicks=0,processStagingWaitTicks=0,handoffWaitTicks=0;
  unsigned processCalls=0;
  bool frozen=false, stagingFence=false;
  size_t stagedBytes=0;
  unsigned heapChecks=0; size_t heapPeakUse=0; // (Claude) real-heap fence: checks this frame, highest in-use seen
  char heaps[256]={};
};
// CPU-only recording; no Renderer3ds instance or Citro3D is touched here.
class FrameRecorder3ds : public GxSink {
  using Ev=RecordedEvent3ds;
  GxFifo fifo_{this};
  static GxState& eventState(Ev& x){
#ifdef N3DS_RECORD_FRAME_TYPES_HEADER
    return x.st;
#else
    return x.state();
#endif
  }
  static bool drawEvent(const Ev& x){
#ifdef N3DS_RECORD_FRAME_TYPES_HEADER
    return x.kind==Ev::Draw;
#else
    return x.isDraw();
#endif
  }

#ifndef N3DS_RECORD_FRAME_TYPES_HEADER
  std::shared_ptr<const DrawRegisterSignature> signatureProof_,learningSignature_;
  uint32_t proofRevision_=0,learningRevision_=0;
  std::shared_ptr<DirectGroupRegistry> groupRegistry_;
  std::shared_ptr<const DirectGroupBinding> activeGroup_;size_t activeGroupRole_=0;
  std::unique_ptr<StateTemplateCache> stateTemplates_;
  std::shared_ptr<DirectDrawRegistry> directRegistry_;
  bool directRoute_=false,directVerify_=false,directSnapshots_=false;
  DirectDrawRegistry::View directView_;DirectGroupRegistry::View groupView_;
#endif
  GxFifo::GuestRead guestRead_;
  bool appendNew_=false;
  bool vertexPoolFifo_=false;
  bool compactDraws_=false;
  void compactPrevious(){
    if(!compactDraws_||fifo_.deferredDecodeActive()||current->events.empty())return;
    Ev& e=current->events.back();if(!drawEvent(e)||e.geo)return;
    current->compactSavedBytes+=compactDrawStorage(e.d);++current->compactBatches;
  }

  // (Claude v83) translate-once: retained geometry (hw6 gxretain.h) in the recorder. A batch whose inputs (draw commands, vertex format, stream bytes,
  // array bindings and every array byte its indices reach) equal those of a batch of the previous frame is not decoded: its event carries the shared,
  // immutable decoded geometry (Ev::geo, read by the render thread). Exact: the inputs are compared when decoding would have read them.
  GxRetainer retainer_;
  bool retainOn_=false, skipped_=false;
  int skipMaxPm_=-1;
  void closeRetained(){
    if(!retainOn_||!retainer_.open())return;
    if(!current->events.empty()&&drawEvent(current->events.back())){
      Ev& x=current->events.back();
      if(auto g=retainer_.closeBatch(x.d,x.maxPm)){current->retainedBytes+=g->bytes;x.geo=std::move(g);}
    } else {GxDraw none;retainer_.closeBatch(none,-1);}
  }
  bool statePoolEnabled_=false;
  bool notes_=true, inPlace_=false;   // (Claude) as Renderer3ds::setStateNotes / setStateInPlace: exact
  // State of a batch: the tracked max position matrix replaces a rescan of the batch's matrix indices (equal: every vertex carries one when the VCD has them).
  // (Claude) state_incremental: every state decode takes the fifo's dirty register groups; a fresh batch's state is then made from the
  // previous decoded state of this frame (lastState_) and only the sections whose registers changed (gxstate.h decodeStateIncremental).
  bool incState_=false;
  bool directDiag_=false;
  const GxState* lastState_=nullptr;   // in current->events (a deque: growing it leaves the elements where they are); reset by select()
  void stateInto(Ev& x, const GxRegs& r, bool fresh) {
#ifndef N3DS_RECORD_FRAME_TYPES_HEADER
    if(!fresh && x.kind==Ev::DirectDraw){x.legacy();lastState_=nullptr;}
    if(fresh&&directRegistry_&&directDiag_){++current->directDiag[0];if(directRoute_)++current->directDiag[1];else if(x.maxPm>(groupRegistry_?9:0))++current->directDiag[2];}
    if(fresh&&directRegistry_&&!directRoute_&&x.maxPm<=(groupRegistry_?9:0)){
      auto geometry=retainer_.matchingGeometry();
      if(!geometry&&directDiag_)++current->directDiag[3];
      if(geometry){
        const auto revision=fifo_.staticRevision();
        const auto proven=revision&&revision==proofRevision_?signatureProof_:nullptr;
        std::shared_ptr<const DirectGroupBinding> group;
        std::shared_ptr<const DirectDrawBinding> binding;
        if(groupRegistry_){
          if(activeGroup_&&activeGroupRole_<activeGroup_->members.size()){
            const auto& next=activeGroup_->members[activeGroupRole_];
            if(next->geometry==geometry->id&&next->geometryOwner.lock()==geometry&&(next->signature==proven||next->signature->matches(r))){group=activeGroup_;binding=next;}
          }
          if(!binding){activeGroup_.reset();activeGroupRole_=0;group=groupRegistry_->find(geometry->id,geometry,r,proven,groupView_);if(group)binding=group->members[0];}
        }
        const bool wanted=directRegistry_->wanted(geometry->id,geometry,directView_);
        if(!binding&&wanted)binding=directRegistry_->find(geometry->id,geometry,r,proven,directView_);
        if(directDiag_&&!binding){if(!wanted)++current->directDiag[4];unsigned reason=groupRegistry_?groupRegistry_->diagnose(geometry->id,geometry,r):0;if(!reason)reason=directRegistry_->diagnose(geometry->id,geometry,r);++current->directDiag[reason?5+reason:5];}
        if(binding&&binding->recipe){
          signatureProof_=binding->signature;proofRevision_=revision;
          const auto& recipe=*binding->recipe;const size_t at=current->directRows.size(),count=recipe.destinationRows;
          if(at+count<=65536){try{
            current->directRows.resize(at+count);
            float projection[4][4];directProjection(*binding,r.xf,projection);
            const int selected=x.d.vcd.pmIdx?0:int(r.xf[0x1018]&63)/3;
            if(applyRawRecipe(recipe,current->directRows.data()+at,count,r.xf,sizeof(r.xf)/sizeof(r.xf[0]),selected,projection,binding->scale)){
              if(directVerify_){
                auto oracle=std::make_unique<GxState>();decodeStateInto(*oracle,r,&x.d,false,x.maxPm);
                auto packed=std::make_unique<PackedUniforms>();
                packUniformsPacked(*packed,binding->compiled->vtx,binding->compiled->vs,oracle->uniformView(),projection,binding->scale);
                bool exact=true;for(const auto& range:binding->ranges){const auto& array=packed->arrays[range.array];if(array.count<int(range.first+range.count)||std::memcmp(array.rows+range.first,current->directRows.data()+at+range.destination,sizeof(PVec)*range.count))exact=false;}
                ++current->directChecks;if(!exact){++current->directMismatches;n3dsLog("[direct-verify] shader rows differ\n");}
              }
              x.setDirect(binding,uint32_t(at),uint32_t(count));
              auto& payload=x.stateStorage.payload();payload.groupOwner=group;payload.guardColors[0]=r.xf[0x100a];payload.guardColors[1]=r.xf[0x100c];
              if(group){activeGroup_=group;activeGroupRole_=size_t(binding->groupRole)+1;if(activeGroupRole_==group->members.size()){activeGroup_.reset();activeGroupRole_=0;}}
              std::memcpy(x.stateStorage.payload().projectionXY,r.xf+0x1020,16);
              // Keep the dirty watermark anchored to the last full state. Direct
              // events read live registers independently; the next ordinary event
              // must see the UNION of intervening changes, not force a full decode.
              if(directDiag_)++current->directDiag[15];
              ++current->directCaptured;return;
            }
            current->directRows.resize(at);
          }catch(const std::bad_alloc&){current->directRows.resize(at);}}
          ++current->directRejected;
        }
        if(wanted&&(groupRegistry_||directRegistry_->canLearn(geometry->id,directView_)))try{
          if(!revision||revision!=learningRevision_||!learningSignature_){learningSignature_=std::make_shared<DrawRegisterSignature>(r,true);learningRevision_=revision;}
          x.rawSignature=learningSignature_;
        }catch(const std::bad_alloc&){}
      }
    }
#endif
    const uint32_t dirty = fifo_.takeStateDirty();
    const GxState* prev = (incState_ && fresh && inPlace_ && lastState_ != &eventState(x)) ? lastState_ : nullptr;
    // (Claude v78) x.dirtyIn: the register groups changed since the previous Draw event's state, valid only when this state was made from it
    // (decodeStateIncremental copies every other section from *prev). A refresh of the same event adds what it took (normally nothing: same generation).
    if (fresh) x.dirtyIn = prev ? dirty : kStateDirtyAll;
    else x.dirtyIn |= dirty;
    if (fresh && inPlace_) {
      if(statePoolEnabled_)current->statePool.take(eventState(x));
#ifndef N3DS_RECORD_FRAME_TYPES_HEADER
      const GxState* material=stateTemplates_&&!notes_?stateTemplates_->find(r):nullptr;
      if(material){decodeStateTemplate(eventState(x),r,&x.d,x.maxPm,dirty,prev,*material);++current->templateHits;}
      else {
#endif
      if (prev) decodeStateIncremental(eventState(x), r, &x.d, notes_, x.maxPm, dirty, prev);
      else decodeStateInto(eventState(x), r, &x.d, notes_, x.maxPm);
#ifndef N3DS_RECORD_FRAME_TYPES_HEADER
      if(stateTemplates_&&!notes_){++current->templateMisses;try{stateTemplates_->remember(r,eventState(x));}catch(const std::bad_alloc&){}}
      }
#endif
    }
    else eventState(x) = decodeState(r, &x.d, notes_, x.maxPm);
    lastState_ = &eventState(x);
  }
  size_t stagingBudget_=2u<<20;
  uint64_t lastDrawGen_=~0ull;
  uint32_t lastAddr_[8]={},lastImage_[8]={},lastMip_[8]={};
  bool seen_[8]={};
  template<class V> static size_t cb(const V& v){return v.capacity()*sizeof(typename V::value_type);}
  static size_t drawCapBytes(const GxDraw& d){
    const auto& w=d.v;size_t n=cb(w.pos)+cb(w.nrm)+cb(w.pmIdx)+cb(d.tris);
    for(int c=0;c<2;++c)n+=cb(w.clr[c]);
    for(int t=0;t<8;++t)n+=cb(w.tex[t])+cb(w.texMtxIdx[t]);return n;
  }
  // Bound speculative recording while the previous frame still owns its data.
  // Charge event/state storage and twice the maximum decoded vertex footprint
  // (vector capacity growth). Large frames continue only after owner completion.
  // (Claude) With a heap probe set, an estimate past the budget fences only when the real heap is short:
  // the probe is re-asked every kHeapStep of further estimate (real growth is at most the estimate) and
  // returns false (fence) near the ceiling. No probe: the estimate alone fences, as before.
  static constexpr size_t kHeapStep=512u<<10;
  bool (*heapRoom_)(size_t* inUse)=nullptr;
  // (Claude v67, staging_partial) heapReady_: true once the render thread has freed enough (free blocks inside the heap + sbrk room, with an sbrk
  // reserve kept for large blocks). With it, a short heap waits only until that is true instead of for the whole previous frame, and is re-checked
  // every kHeapStep of further estimate. Timing only: what is recorded and rendered is unchanged.
  bool (*heapReady_)()=nullptr;
  bool partialWait(){   // true: room appeared while the owner still works; false: the owner went idle (same state as after stagingSync)
#ifdef __3DS__
    const uint64_t start=svcGetSystemTick();
#endif
    bool partial=false;
    while(n3dsRenderThreadBusy()){ if(heapReady_()){partial=true;break;} svcSleepThread(300000); }
    if(!partial) n3dsGpuSync();
#ifdef __3DS__
    const uint64_t ticks=svcGetSystemTick()-start;
    current->stagingWaitTicks+=ticks;
    if(inProcess)current->processStagingWaitTicks+=ticks;
#endif
    return partial;
  }
  size_t heapCheckAt_=0;
  void stagingSync(){
#ifdef __3DS__
    const uint64_t start=svcGetSystemTick();
#endif
    n3dsGpuSync();
#ifdef __3DS__
    const uint64_t ticks=svcGetSystemTick()-start;
    current->stagingWaitTicks+=ticks;
    if(inProcess)current->processStagingWaitTicks+=ticks;
#endif
  }
  void charge(size_t bytes) {
    const size_t budget=stagingBudget_;
    if(!current->stagingFence && bytes>budget-std::min(budget,current->stagedBytes)) {
      const size_t after=current->stagedBytes+bytes;
      if(!heapRoom_) { stagingSync();current->stagingFence=true; }
      else if(after>=heapCheckAt_) {
        size_t use=0;const uint64_t hp0=t2pProfTicks();const bool room=heapRoom_(&use);if(t2pProfEnabled())g_t2pProf[4]+=t2pProfTicks()-hp0;
        ++current->heapChecks;current->heapPeakUse=std::max(current->heapPeakUse,use);
        if(room || (heapReady_ && heapReady_())) heapCheckAt_=after+kHeapStep;
        else if(heapReady_) { if(partialWait()) heapCheckAt_=after+kHeapStep; else current->stagingFence=true; }
        else { stagingSync();current->stagingFence=true; }
      }
    }
    current->stagedBytes+=bytes;
  }
  // (Claude v79) collect reads only BP texture registers, bpWritten and texAddr, all of which bump the fifo's state generation when they change: with
  // the same generation since the last collect of this frame (seen_ unchanged), it would add nothing.
  uint64_t collectGen_=~0ull;
  void collect(const GxRegs& r) {
    const uint64_t g=fifo_.stateGeneration();
    if(g==collectGen_)return;
    collectGen_=g;
    for(int m=0;m<8;++m){
      const int off=(m<4?m:m-4)+(m<4?0:0x20);
      if(!r.bpWritten[0x88+off])continue;
      uint32_t image=r.bp[0x88+off]&0xffffff,mode=r.bp[0x80+off]&0xffffff;
      uint32_t addr=r.texAddr[m]?r.texAddr[m]:(r.bp[0x94+off]&0x1fffff)<<5;
      if(!addr)continue;
      const uint32_t mip=(mode&0x60)|(r.bp[0x84+off]&0xff00);
      if(seen_[m] && lastAddr_[m]==addr && lastImage_[m]==image && lastMip_[m]==mip)continue;
      seen_[m]=true;lastAddr_[m]=addr;lastImage_[m]=image;lastMip_[m]=mip;
      GxTexBinding b;b.width=(image&1023)+1;b.height=((image>>10)&1023)+1;
      b.fmt=(image>>20)&15;b.mipMode=(mode>>5)&3;b.maxLod=float((r.bp[0x84+off]>>8)&255)/16.f;
      size_t n=0;int w=b.width,h=b.height;
      for(int l=0;l<b.mipLevels();++l){n+=gxTextureBytes(b.fmt,w,h);w=std::max(1,w/2);h=std::max(1,h/2);}
      current->snapshot.add(addr,n<=UINT32_MAX?uint32_t(n):0);
    }
  }
public:
  FrameRecorder3ds() {
#ifdef __3DS__
    n3dsTypedRegisterFifo(&fifo_);
#endif
  }
  ~FrameRecorder3ds() {
#ifdef __3DS__
    n3dsTypedUnregisterFifo(&fifo_);
#endif
  }
  bool inProcess=false; // producer-only; separates decode work from staging waits
  CpuFrame3ds* current=nullptr;
  uint32_t primitiveRunLimit() const override {
    if(!current)return 0;
    if(!current->events.empty()&&drawEvent(current->events.back())&&!current->events.back().geo&&lastDrawGen_==fifo_.stateGeneration()&&gxTriCount(current->events.back().d)!=0)
      return kBatchMaxVertices-std::min<uint32_t>(kBatchMaxVertices,current->events.back().d.v.count);
    return kBatchMaxVertices;
  }
  GxFifo& fifo(){return fifo_;}
  void setGuestRead(GxFifo::GuestRead reader){guestRead_=std::move(reader);fifo_.setGuestArrayReader(guestRead_);}
  void setStateNotes(bool on){notes_=on;}
  void setVertexPoolFifo(bool on){vertexPoolFifo_=on;}
  void setCompactDrawStorage(bool on){compactDraws_=on;}
  void setStatePool(bool on){statePoolEnabled_=on;}
  void setStateInPlace(bool on){inPlace_=on;}
  void setStateIncremental(bool on){incState_=on;}
  void setStateTemplates(bool on){
#ifndef N3DS_RECORD_FRAME_TYPES_HEADER
    if(on&&!stateTemplates_)stateTemplates_=std::make_unique<StateTemplateCache>();else if(!on)stateTemplates_.reset();
#endif
  }
#ifndef N3DS_RECORD_FRAME_TYPES_HEADER
  void setGroupRegistry(std::shared_ptr<DirectGroupRegistry> registry){groupRegistry_=std::move(registry);}
  void setDirectSnapshots(bool on){directSnapshots_=on;}
  void setDirectDiagnostics(bool on){directDiag_=on;}
  void setDirectRegistry(std::shared_ptr<DirectDrawRegistry> registry,bool verify=false){directRegistry_=std::move(registry);directVerify_=verify;fifo_.setStaticTracking(bool(directRegistry_));}
#endif
  void setRetainGeometry(bool on,size_t budget){if(on==retainOn_)return;if(current)closeRetained();retainOn_=on;fifo_.setRetainOffers(on);retainer_.setBudget(budget);if(!on)retainer_.clear();}
  GxRetainStats takeRetainStats(){return retainer_.takeStats();}
  bool retainOffer(const GxDrawInputs& in,GxDraw& D) override{
    int m=-1;const uint64_t t=t2pProfTicks();
    skipped_=retainer_.offer(in,D,appendNew_,fifo_.processSerial(),m);
    if(t2pProfEnabled())g_t2pProf[7]+=t2pProfTicks()-t;
    skipMaxPm_=m;return skipped_;
  }
  void retainDecoded(const GxDrawInputs& in,const GxDraw& D,uint32_t firstVertex,size_t firstTri,bool arrayError) override{
    retainer_.decoded(in,D,firstVertex,firstTri,arrayError,fifo_.processSerial());
  }
  void setHeapProbe(bool (*probe)(size_t*)){heapRoom_=probe;}
  void setHeapReady(bool (*ready)()){heapReady_=ready;}
  void select(CpuFrame3ds* frame, size_t testBudget=0){
#ifndef N3DS_RECORD_FRAME_TYPES_HEADER
    directView_=directSnapshots_&&directRegistry_?directRegistry_->readView():DirectDrawRegistry::View{};groupView_=directSnapshots_&&groupRegistry_?groupRegistry_->readView():DirectGroupRegistry::View{};
    directRoute_=false;signatureProof_.reset();learningSignature_.reset();proofRevision_=learningRevision_=0;activeGroup_.reset();activeGroupRole_=0;std::fill(std::begin(frame->directDiag),std::end(frame->directDiag),0);frame->directDiag[14]=unsigned(bool(directView_)&&bool(groupView_));frame->directRows.clear();frame->directCaptured=frame->directRejected=frame->directChecks=frame->directMismatches=0;
#endif
current=frame;current->templateHits=current->templateMisses=0;lastState_=nullptr;stagingBudget_=(testBudget==(2u<<20)||testBudget==(8u<<20))?testBudget:n3dsRenderStagingBudget();lastDrawGen_=~0ull;collectGen_=~0ull;for(bool& b:seen_)b=false;current->stagingWaitTicks=current->processStagingWaitTicks=current->handoffWaitTicks=0;current->stagedBytes=0;current->stagingFence=false;current->compactSavedBytes=0;current->compactBatches=0;current->heapChecks=0;current->heapPeakUse=0;heapCheckAt_=0;}
  void onDraw(const GxRegs&,const GxDraw&) override;
  GxDraw* appendTarget(const GxRegs&,const GxDraw&) override;
  void onAppended(const GxRegs&,GxDraw&,uint32_t) override;
  void onCopy(const GxRegs&) override;
  void onTlutLoad(const GxRegs&) override;
  void onUiRoute(const GxRegs&,const GxUiRoute&) override;   // (Claude v80) ordered route marker: a Route event (also a batch boundary)
  void onError(const std::string& e) override {n3dsLog("[recorder] %s\n",e.c_str());}
  const uint8_t* resolveDisplayList(uint32_t a,uint32_t n) override{return guestRead_?guestRead_(a,n):reinterpret_cast<const uint8_t*>(uintptr_t(a));}
};
inline void FrameRecorder3ds::onDraw(const GxRegs& r, const GxDraw& d) {
  charge(8192+size_t(d.v.count)*256);
  closeRetained();   // (as Renderer3ds::onDraw) an inline draw ends the open retained batch
  
  collect(r);
  const uint64_t gen = fifo_.stateGeneration();
  if (kBatchMaxVertices && !current->events.empty() && drawEvent(current->events.back()) && !current->events.back().geo && gen == lastDrawGen_ && canAppend(current->events.back().d, d, kBatchMaxVertices)) {
    Ev& x = current->events.back();
    const uint64_t ta = t2pProfTicks();
    const int before = x.maxPm, mine = maxPosMtx(d);
    appendDraw(x.d, d);
    if (t2pProfEnabled()) g_t2pProf[5] += t2pProfTicks() - ta;
    x.merged = x.d.commandCount;
    if (mine > before) {
      x.maxPm = mine; const uint64_t t = t2pProfTicks(); stateInto(x, r, false); if (t2pProfEnabled()) g_t2pProf[3] += t2pProfTicks() - t; }   // more position matrices are indexed now (GxDrawState::numPosMtx)
    return;
  }
  closeRetained();
  compactPrevious();
  const uint64_t t0 = t2pProfTicks();
  charge(8192);
  current->events.emplace_back();
  Ev& x = current->events.back();
  x.kind = Ev::Draw;
  x.d = d;
  x.merged = d.commandCount;
  x.maxPm = maxPosMtx(d);
  const uint64_t t1 = t2pProfTicks();
  stateInto(x, r, true);
  if (t2pProfEnabled()) g_t2pProf[3] += t2pProfTicks() - t1;
  if (t2pProfEnabled()) g_t2pProf[4] += t1 - t0;
  lastDrawGen_ = gen;
}

inline GxDraw* FrameRecorder3ds::appendTarget(const GxRegs& r, const GxDraw& h) {
  const uint64_t q0=t2pProfTicks();
  charge(size_t(h.vertexCount)*256);
  
  collect(r);
  const uint64_t gen = fifo_.stateGeneration();
  if (kBatchMaxVertices && !current->events.empty() && drawEvent(current->events.back()) && !current->events.back().geo && gen == lastDrawGen_ && canAppendHeader(current->events.back().d, h, kBatchMaxVertices)) {
    appendNew_ = false;
    if(t2pProfEnabled())g_t2pProf[8]+=t2pProfTicks()-q0;
    return &current->events.back().d;
  }
  const uint64_t q1=t2pProfTicks();
  closeRetained();
  compactPrevious();
  charge(8192);
  const uint64_t q2=t2pProfTicks();
  current->events.emplace_back();
  Ev& x = current->events.back();
  if(t2pProfEnabled()){g_t2pProf[12]+=q2-q1;g_t2pProf[13]+=t2pProfTicks()-q2;}
  if(t2pProfEnabled()){const uint64_t t=t2pProfTicks();g_t2pProf[9]+=q1-q0;g_t2pProf[10]+=t-q1;g_t2pProf[11]++;}
  x.kind = Ev::Draw;
  if (!current->drawPool.empty()) {
    auto& p=current->drawPool;const size_t at=vertexPoolFifo_?current->drawPoolRead:p.size()-1;
    current->drawPoolBytes-=std::min(current->drawPoolBytes,drawCapBytes(p[at]));
    x.d=std::move(p[at]);
    if(vertexPoolFifo_){if(++current->drawPoolRead==p.size()){p.clear();current->drawPoolRead=0;}}
    else p.pop_back();
  }
  x.d.commandCount = h.commandCount; x.merged = h.commandCount; x.d.prim = h.prim; x.d.vatIdx = h.vatIdx; x.d.vertexCount = h.vertexCount; x.d.unsupportedPrim = h.unsupportedPrim; x.d.vcd = h.vcd; x.d.vat = h.vat;
  x.d.retained = false; x.d.retainedTris = 0;
  appendNew_ = true;
  lastDrawGen_ = gen;
  return &x.d;
}

extern "C" uint64_t g_n3dsDecodeCount[2];
inline void FrameRecorder3ds::onAppended(const GxRegs& r, GxDraw& into, uint32_t firstVertex) {
  Ev& x = current->events.back();
  const int mine = skipped_ ? skipMaxPm_ : maxPosMtxFrom(into, firstVertex);   // a retained (skipped) draw has no vertices here: the retainer knows its maximum
  skipped_ = false;
  const uint64_t t = t2pProfTicks();
  if (appendNew_) {
    ++g_n3dsDecodeCount[0];
    x.maxPm = mine;
    stateInto(x, r, true);
  } else {
    x.merged = x.d.commandCount;
    if (mine > x.maxPm) { ++g_n3dsDecodeCount[1]; x.maxPm = mine; stateInto(x, r, false); }   // more position matrices are indexed now (GxDrawState::numPosMtx)
  }
  if (t2pProfEnabled()) g_t2pProf[3] += t2pProfTicks() - t;
}

inline void FrameRecorder3ds::onCopy(const GxRegs& r) {
  closeRetained();
  compactPrevious();
  charge(8192);
  current->events.emplace_back();
  Ev& x = current->events.back();
  x.c = decodeCopy(r);
  fifo_.takeStateDirty();
  eventState(x) = decodeState(r, nullptr);
  lastState_ = nullptr;   // (Claude v61) a copy decodes with temporary registers (Aurora Z copy switches bp 0x43 to Z24, then restores it untracked): the next draw decodes in full
  x.kind = x.c.toXfb ? Ev::Xfb : Ev::TexCopy;
  if (x.c.toXfb) { current->framesReady++; if (retainOn_) retainer_.endFrame(); }
}

inline void FrameRecorder3ds::onUiRoute(const GxRegs&, const GxUiRoute& route) {
  closeRetained();
  compactPrevious();
  charge(2048);
  current->events.emplace_back();
  Ev& x = current->events.back();
  x.kind = Ev::Route;
  x.route = route;
#ifndef N3DS_RECORD_FRAME_TYPES_HEADER
  directRoute_=route.begin;
#endif
}

inline void FrameRecorder3ds::onTlutLoad(const GxRegs& r) {
  closeRetained();
  compactPrevious();
  charge(8192);
  current->events.emplace_back();
  Ev& x = current->events.back();
  x.kind = Ev::Tlut;
  x.tl = decodeTlutLoad(r);
  const size_t n = size_t(x.tl.entries) * 2;
  charge(n);
  if (n) {
    const uint8_t* src = guestRead_?guestRead_(x.tl.srcAddr,uint32_t(n)):reinterpret_cast<const uint8_t*>(uintptr_t(x.tl.srcAddr));
    if (src) x.tlutBytes.assign(src, src + n);
  }
}

} // namespace t2p
