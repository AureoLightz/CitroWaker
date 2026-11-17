#include "matrixbatch.h"
void n3dsFastAllocEnable(bool on, int maxSpans = 0);   // main_3ds.cpp (Claude v76)
// Diagnostic-only opt-in entry. Not included in the player entry point.
#pragma once
#include <citro3d.h>
#include <cstdarg>
#include <malloc.h>
#include <unistd.h>
extern "C" char* fake_heap_end;
#include <memory>
#include <string>
#include <vector>
#include "record_frame_3ds.h"
#include "packet_worker3ds.h"
#include "typed_gx_replay_3ds.h"
extern "C" void C3Di_RenderQueueWaitDone(void);
void JKRPcBeginHostAlloc();
void JKRPcEndHostAlloc();
#include "model_command_test_3ds.h"
namespace core2test {
static bool directGroupColdTest=false;
static bool copyReuseTest=false;static bool directGroupsTest=false;static bool directSnapshotTest=false;
static bool stateTemplatesTest=false;
constexpr const char* dir=N3DS_ROOT "/claude-replay/";
FILE* log=nullptr;
int directSlotsTest=128;int paletteTest=0;bool depthTest=false;
bool stateProgramTest=false,modelCommandTest=false;
bool bindingMemoTest=true;
bool rigidClipTest=false,textureStoreTest=false;
bool planTemplateTest=false;
bool packetWorkerTest=false,packetWorkerVerifyTest=false;
bool stateIncTest=false,compactTest=false,texErrTest=false,reuseTest=false;   // (Claude) recorder state_incremental
bool matrixBatchTest=false,matrixCostTest=false,splitTest=false,plainTest=false,recProfTest=false;
bool preparedShapeTest=false,preparedDrawTest=false,directVerifyTest=false,directEventsTest=false,modelProgramTest=false,kick48Test=false,uniformDeltaTest=false,commandBlocksTest=false,replayTest=false;
bool heapTest=false,typedTest=false,directTest=false,poolTest=false,fifoPoolTest=false,costTest=false,quietPlanTest=false,directPackTest=false;
bool replayHeapRoom(size_t* use){auto mi=mallinfo();*use=size_t(mi.uordblks);char* b=(char*)sbrk(0);return *use<=(85u<<20) && b && b!=(char*)-1 && b<fake_heap_end && size_t(fake_heap_end-b)>=(3u<<20);}
void out(const char* fmt, ...) {
  va_list ap;va_start(ap,fmt);vfprintf(log,fmt,ap);va_end(ap);fflush(log);
}
void mark(const char* what){   // (Claude v66) progress marker with the heap's state, so a console stop shows where
  auto mi=mallinfo();char* b=(char*)sbrk(0);
  out("STEP %s in-use %u KB free-in-heap %u KB sbrk-room %ld KB\n",what,unsigned(mi.uordblks>>10),unsigned(mi.fordblks>>10),b&&b!=(char*)-1?long((fake_heap_end-b)>>10):-1L);
}
std::vector<uint8_t> load(const std::string& path){
  std::vector<uint8_t> v;
  if(FILE* f=fopen(path.c_str(),"rb")){fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    if(n>0){try{v.resize(n);}catch(...){if(log)out("ERROR cannot allocate %ld bytes for %s\n",n,path.c_str());fclose(f);return {};}if(fread(v.data(),1,v.size(),f)!=v.size())v.clear();}fclose(f);}
  return v;
}
uint32_t word(const std::vector<uint8_t>& v,size_t& at){
  uint32_t x=0;if(at+4<=v.size())memcpy(&x,v.data()+at,4);at+=4;return x;
}
struct Memory:t2p::GuestMem {
  std::vector<uint8_t> bytes;
  RenderSnapshot3ds* view=nullptr;
  unsigned misses=0, verifyMisses=0;
  bool verifying=true;
  const uint8_t* live(uint32_t a,size_t n) const {
    a&=0x03ffffff;return uint64_t(a)+n<=bytes.size()?bytes.data()+a:nullptr;
  }
  const uint8_t* read(uint32_t a,size_t n) const override {
    if(!view)return live(a,n);
    const uint8_t* p=view->read(a,n);
    if(!p){auto* self=const_cast<Memory*>(this);if(verifying)++self->verifyMisses;else ++self->misses;}
    return p;
  }
};
struct Slot:t2p::CpuFrame3ds {std::vector<std::pair<uint32_t,uint32_t>> dirty;};
struct Context {
  Memory mem;
  std::unique_ptr<t2p::Renderer3ds> r;
  C3D_RenderTarget* target=nullptr;
  const std::vector<uint8_t>* bundle=nullptr;
  Slot* submitted=nullptr;
  std::string capture;
  int mode=0,pass=0,frame=0;
  uint64_t clipTests=0,clipCulled=0;
  unsigned fallbacks=0,errors=0,heapChecks=0,fences=0;
  bool okay=false;
  uint64_t freezeTicks=0,syncTicks=0,stagingTicks=0,renderTicks=0,processTicks=0,cost[16]={},pcost[16]={};unsigned costFrames=0;
  t2p::TypedReplayCounts typedCounts;
  t2p::DrawCompileCache paletteCache{256};unsigned paletteChanged=0,paletteLit=0,paletteSkipped=0;
};
void init(void* ptr){
  auto& c=*static_cast<Context*>(ptr);
  if(!C3D_Init(0x200000))return;
  c.target=C3D_RenderTargetCreate(240,400,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
  if(!c.target)return;
  C3D_RenderTargetSetOutput(c.target,GFX_TOP,GFX_LEFT,GX_TRANSFER_FLIP_VERT(0)|GX_TRANSFER_OUT_TILED(0)|GX_TRANSFER_RAW_COPY(0)|
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)|GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
  t2p::RendererOptions o;
  o.frame.efbW=640;o.frame.efbH=480;o.frame.targetW=400;o.frame.targetH=240;
  o.frame.fit=t2p::FitMode::Stretch;o.frame.widescreen=true;o.frame.rotate=true;
  o.materialCacheFastPath=true;o.translate.packedUniforms=true;o.texCache.memoPalettes=true;
  o.packetNotes=false;o.texCache.budgetBytes=8u<<20;o.texCache.directEncode=directTest;o.texCache.fastLossyError=texErrTest;
  c.r=std::make_unique<t2p::Renderer3ds>();
  if(!c.r->init(o,&c.mem,c.target))return;
  c.r->fifo().setGuestArrayReader([&c](uint32_t a,uint32_t n){return c.mem.live(a,n);});
  c.r->setDisplayListResolver([&c](uint32_t a,uint32_t n){return c.mem.live(a,n);});
  if(preparedShapeTest&&!c.r->fifo().loadPreparedShapes(N3DS_ROOT "/ww_shapes.bin")){out("ERROR prepared shapes load failed\n");return;}
  c.r->fifo().setAuroraMode(true);c.r->fifo().setPrimitiveRuns(true);c.r->fifo().setGeometryCache(false);
  c.r->fifo().setFastAttributeDecode(true);c.r->fifo().setCachedVertexPlan(true);
  c.r->setPacketWorker(packetWorkerTest);c.r->setPacketWorkerVerify(packetWorkerVerifyTest);c.r->setPlanTemplates(planTemplateTest);c.r->backend().setTextureBindingMemo(bindingMemoTest);c.r->setMatrixBatch(matrixBatchTest);c.r->setDrawReuse(reuseTest);c.r->setMatrixBatchProfile(matrixCostTest);c.r->setFrameTiming(splitTest);c.r->setPlanReuse(true);c.r->setFogPreparation(true);c.r->setStateNotes(false);c.r->setStateInPlace(true);c.r->setStatePool(poolTest);c.r->setVertexPoolFifo(fifoPoolTest);c.r->setPlanNotes(!quietPlanTest);c.r->setDirectPack(directPackTest);
  c.r->setCacheVerifiedHook([&c]{c.mem.verifying=false;});
  if(paletteTest>1)c.r->setDrawEdit([&c](t2p::GxState& s,t2p::GxDraw& d,int){
    if(s.proj.ortho || s.draw.numPosMtx!=1 || !d.v.hasPos || !d.v.count) return;
    if(d.v.hasPmIdx)for(auto k:d.v.pmIdx)if(k!=0)return;
    for(int t=0;t<8;t++)if(d.v.hasTexMtxIdx[t])return;
    t2p::Options o;o.verifySamples=0;o.approxEffort=0;o.frameFogRange=160000;
    auto gs=s.draw;gs.numPosMtx=paletteTest;gs.vtx.numPosMtx=1;
    auto sh=c.paletteCache.get(gs,o);
    if(sh->overall==t2p::Fidelity::Unsupported || !sh->vs.ok || sh->pass2 || !sh->vs.usesMtxIdxAttr || sh->vsUniformRegs>96 || sh->attrs.size()>12){++c.paletteSkipped;return;}
    auto u=std::make_shared<t2p::VtxUniforms>(s.vertexUniforms());
    const int selected=paletteTest-1;
    // Distinct unused slots catch accidental use of slot0 instead of selected.
    for(int k=1;k<paletteTest;k++) {
      memcpy(u->pos[k],u->pos[0],sizeof u->pos[k]);memcpy(u->nrm[k],u->nrm[0],sizeof u->nrm[k]);
    }
    u->pos[0][0][3]+=1234.0f;u->nrm[0][0][0]+=7.0f;
    s.uniformBlock=u;s.draw=std::move(gs);
    d.v.hasPmIdx=true;d.v.pmIdx.resize(d.v.count);std::fill(d.v.pmIdx.begin(),d.v.pmIdx.end(),uint8_t(3*selected));
    ++c.paletteChanged;if(sh->vs.usesNormal)++c.paletteLit;
  });

  if(textureStoreTest&&!c.r->loadTexturePack(N3DS_ROOT "/ww_texpack",true,true)){out("ERROR split texture store unavailable\n");return;}
  if(preparedDrawTest && !c.r->backend().loadPreparedDraw(N3DS_ROOT "/ww_commands.cwcp",true))out("ERROR prepared draw load failed\n");
  c.r->backend().setCopyReuse(copyReuseTest);c.r->setDirectEvents(directEventsTest||directGroupsTest,directGroupsTest?512:directSlotsTest);c.r->setDirectGroups(directGroupsTest,directGroupColdTest);c.r->setDirectSnapshots(directSnapshotTest);
  c.r->setModelPrograms(modelProgramTest);if(kick48Test)c.r->backend().setKickDraws(48);
  c.r->setUniformDelta(uniformDeltaTest);
  c.r->setCommandBlocks(commandBlocksTest);
  if(replayTest){c.r->setResidentGeometry(true,2048u<<10);c.r->setDrawReplay(true);c.r->setDrawRecord(true);}
  c.r->setDrawTimingEnabled(false);c.r->setRetainGeometry(rigidClipTest||replayTest);c.r->setRigidClip(rigidClipTest);
  c.r->setLogs(nullptr,nullptr,[&c](const std::string& s){if(s.rfind("DIRECTLEARN",0)==0||s.rfind("replay tail differs at bytes ",0)==0){out("INFO %s\n",s.c_str());return;}++c.errors;out("ERROR %s\n",s.c_str());});
  std::string err;
  c.okay=c.r->loadMaterialBundle(c.bundle->data(),c.bundle->size(),&err);
  if(!c.okay)out("ERROR bundle %s\n",err.c_str());
}
void render(void* ptr){
  auto& c=*static_cast<Context*>(ptr);
  Slot* s=c.submitted;
  if(s){
    c.heapChecks+=s->heapChecks;c.fences+=s->stagingFence;
    for(auto d:s->dirty)c.r->invalidateTextures(d.first,d.second);
    if(directVerifyTest)out("STATETEMPLATE pass %d frame %d hits %u misses %u\n",c.pass,c.frame,s->templateHits,s->templateMisses);
    if(directSnapshotTest)out("DIRECTSNAP pass %d frame %d active %u\n",c.pass,c.frame,s->directDiag[14]);
    if(directVerifyTest){unsigned signatures=0,geometries=0,single=0,perspective=0,pieces=0;for(const auto& ev:s->events)if(ev.isDraw()){pieces+=bool(ev.viewState().uniformSections&&ev.viewState().uniformSections->pieces);signatures+=bool(ev.rawSignature);geometries+=bool(ev.geo);single+=ev.maxPm<=0;perspective+=!ev.viewState().proj.ortho;}out("DIRECTSOURCE pass %d frame %d signatures %u geo %u single %u perspective %u captured %u checks %u mismatches %u pieces %u\n",c.pass,c.frame,signatures,geometries,single,perspective,s->directCaptured,s->directChecks,s->directMismatches,pieces);}
    c.r->swapRecording(*s);
    if(s->frozen){c.mem.view=&s->snapshot;n3dsRenderThreadRelease();}
    else ++c.fallbacks; // synchronous owner; producer remains fenced
  }
  out("MODELHEAP capture %s pass %d frame %d inuse %u free %u\n",c.capture.c_str(),c.pass,c.frame,unsigned(mallinfo().uordblks),unsigned(mallinfo().fordblks));
  c.mem.verifying=true;
  const bool measure=costTest&&(c.mode==0||c.mode==8),prof=measure&&!plainTest;   // (Claude v77) plain=1: render time without the per-draw profiling ticks
  if(prof)for(auto& v:g_t2pProf)v=0;
  const bool oldProf=g_t2pProfOn;g_t2pProfOn=prof;
  const uint64_t renderStart=svcGetSystemTick();
  auto rep=c.r->renderFrame(true);C3Di_RenderQueueWaitDone();
  c.clipTests+=rep.clipTests;c.clipCulled+=rep.clipCulled;
  if(preparedDrawTest)out("PREPARED pass %d frame %d builds %llu submits %llu mismatches %llu\n",c.pass,c.frame,(unsigned long long)c.r->backend().preparedBuilds(),(unsigned long long)c.r->backend().preparedSubmits(),(unsigned long long)c.r->backend().preparedMismatches());
  if(copyReuseTest)out("COPYREUSE pass %d frame %d hits %llu live %llu vram %llu\n",c.pass,c.frame,(unsigned long long)c.r->backend().copyReuses(),(unsigned long long)c.r->backend().stats().rtLive,(unsigned long long)c.r->backend().stats().rtVramBytes);
  if(directEventsTest||directGroupsTest)out("DIRECTEVENT pass %d frame %d draws %u fallback %u groups %u\n",c.pass,c.frame,rep.directDraws,rep.directFallbacks,rep.directGroups);
  if(modelProgramTest)out("MODELPROGRAM pass %d frame %d hits %u draws %u reject %u %u %u %u %u %u\n",c.pass,c.frame,rep.modelProgramHits,rep.modelProgramDraws,rep.modelProgramReject[0],rep.modelProgramReject[1],rep.modelProgramReject[2],rep.modelProgramReject[3],rep.modelProgramReject[4],rep.modelProgramReject[5]);
  if(modelProgramTest)out("MODELGROUP pass %d frame %d hits %u members %u\n",c.pass,c.frame,rep.modelGroupPrograms,rep.modelGroupMembers);
  if(uniformDeltaTest)out("UDELTA pass %d frame %d hits %u\n",c.pass,c.frame,rep.uniformDeltaHits);
  if(splitTest){const double k=1000.0/double(SYSCLOCK_ARM11);out("SPLIT pass %d frame %d policy %.3f begin %.3f loop %.3f copies %.3f end %.3f matrix %.3f recycle %.3f popfront %.3f build %.3f plan %.3f draw %.3f\n",c.pass,c.frame,rep.splitTicks[0]*k,rep.splitTicks[1]*k,rep.splitTicks[2]*k,rep.splitTicks[3]*k,rep.splitTicks[4]*k,rep.splitTicks[5]*k,rep.splitTicks[6]*k,rep.splitTicks[7]*k,rep.buildMs,rep.planMs,rep.drawMs);}
  if(splitTest){const double k=1000.0/double(SYSCLOCK_ARM11);out("MSPLIT pass %d frame %d key %.3f appends %.3f variant %.3f calib %.3f groups %llu members %llu\n",c.pass,c.frame,rep.matrixSplit[0]*k,rep.matrixSplit[1]*k,rep.matrixSplit[2]*k,rep.matrixSplit[3]*k,(unsigned long long)rep.matrixSplit[4],(unsigned long long)rep.matrixSplit[5]);}
  if(matrixCostTest){auto& MP=t2p::matrixbatch::prof();out("MBPROF pass %d frame %d why",c.pass,c.frame);for(int i=0;i<t2p::matrixbatch::MB_N;i++)out(" %s=%u",t2p::matrixbatch::kWhy[i],MP.why[i]);out(" np %llu %llu %llu %llu %llu cheap %u/%llu nonpose %u/%llu heavy %u/%llu\n",(unsigned long long)MP.np[0],(unsigned long long)MP.np[1],(unsigned long long)MP.np[2],(unsigned long long)MP.np[3],(unsigned long long)MP.np[4],MP.calls[0],(unsigned long long)MP.ticks[0],MP.calls[1],(unsigned long long)MP.ticks[1],MP.calls[2],(unsigned long long)MP.ticks[2]);MP=t2p::matrixbatch::Prof{};}
  if(matrixCostTest)out("MATRIXCOST pass %d frame %d samples %u attempts %u ticks %llu %llu %llu\n",c.pass,c.frame,rep.matrixSamples,rep.matrixAttempts,(unsigned long long)rep.matrixTicks[0],(unsigned long long)rep.matrixTicks[1],(unsigned long long)rep.matrixTicks[2]);
  if(planTemplateTest)out("PLANCACHE lookups %llu hits %llu misses %llu bytes %u budget %u\n",(unsigned long long)rep.templateStats.lookups,(unsigned long long)rep.templateStats.hits,(unsigned long long)rep.templateStats.misses,unsigned(rep.templateStats.bytes),unsigned(rep.templateStats.budget));
  if(reuseTest&&c.frame%8==0)out("REUSE pass %d frame %d hits %u misses %u inserts %u full %u entries %lu bytes %lu\n",c.pass,c.frame,rep.reuseHits,rep.reuseMisses,rep.reuseInserts,rep.reuseFull,(unsigned long)rep.reuseEntries,(unsigned long)rep.reuseBytes);
  if(packetWorkerTest)out("PACKET_WORKER active %d jobs %u wait %llu checks %u mismatches %u\n",int(c.r->packetWorkerActive()),rep.packetJobs,(unsigned long long)rep.packetWaitTicks,rep.packetChecks,rep.packetMismatches);
  const uint64_t renderEnd=svcGetSystemTick();g_t2pProfOn=oldProf;
  if(measure&&c.pass>0){c.renderTicks+=renderEnd-renderStart;++c.costFrames;if(prof)for(int i=0;i<16;++i)c.cost[i]+=g_t2pProf[i];}
  constexpr size_t n=400*240*4;
  auto* raw=static_cast<const uint8_t*>(c.target->frameBuf.colorBuf);
  GSPGPU_InvalidateDataCache(const_cast<uint8_t*>(raw),n);
  uint32_t hash=2166136261u;for(size_t i=0;i<n;++i)hash=(hash^raw[i])*16777619u;
  out("PIX capture %s mode %d pass %d frame %d hash %08lx draws %u/%u gx %u copies %u/%u miss %u verify-miss %u fallback %u errors %u\n",
    c.capture.c_str(),c.mode,c.pass,c.frame,(unsigned long)hash,unsigned(rep.drawn),unsigned(rep.draws),
    unsigned(rep.gxDraws),unsigned(rep.copiesDone),unsigned(rep.copies),c.mem.misses,c.mem.verifyMisses,c.fallbacks,c.errors);
  if(c.pass==0){
    char name[256];snprintf(name,sizeof name,"%sparity-%s-%d-%02d.rgba",dir,c.capture.c_str(),c.mode,c.frame);
    if(FILE* f=fopen(name,"wb")){if(fwrite(raw,1,n,f)!=n)++c.errors;fclose(f);}else ++c.errors;
  }
  if(depthTest && c.pass==0){
    char name[256];snprintf(name,sizeof name,"%sparity-%s-%d-%02d.depth",dir,c.capture.c_str(),c.mode,c.frame);
    auto* z=c.target->frameBuf.depthBuf;GSPGPU_InvalidateDataCache(z,n);
    if(FILE* f=fopen(name,"wb")){if(fwrite(z,1,n,f)!=n)++c.errors;fclose(f);}else ++c.errors;
  }
  if(s){c.r->swapRecording(*s);s->snapshot.reset();s->dirty.clear();}
  c.mem.view=nullptr;
}
void fini(void* ptr){
  auto& c=*static_cast<Context*>(ptr);
  out("PALETTE changed %u lit %u skipped %u\n",c.paletteChanged,c.paletteLit,c.paletteSkipped);
  C3Di_RenderQueueWaitDone();c.r.reset();if(c.target)C3D_RenderTargetDelete(c.target);C3D_Fini();
}
int phase(const std::string& name,const std::vector<uint8_t>& stream,const std::vector<uint8_t>& bundle,int mode,int passes){
  size_t at=4;uint32_t version=word(stream,at),declared=word(stream,at),chunks=word(stream,at),used=0;
  if(version!=2)return 1;
  for(uint32_t i=0;i<chunks;i++){
    if(at+4>stream.size())return 2;uint8_t type=stream[at];at+=4;
    if(type==0){uint32_t n=word(stream,at);at+=n;}
    else if(type==1){uint32_t a=word(stream,at),n=word(stream,at);if(uint64_t(a)+n>declared)return 3;used=std::max(used,a+n);at+=n;}
    else if(type==2)word(stream,at);else return 4;
    if(at>stream.size())return 5;
  }
  mark("phase begin");
  auto c=std::make_unique<Context>();c->capture=name;c->mode=mode;c->bundle=&bundle;
  out("STEP guest memory %u KB\n",unsigned(((used+65535u)&~65535u)>>10));
  try{c->mem.bytes.assign((used+65535u)&~65535u,0);}catch(...){out("ERROR cannot allocate guest memory\n");return 11;}
  mark("guest memory ready");
  const bool threaded=mode!=0;
  if(threaded){setenv("N3DS_RENDER_THREAD","1",1);if(!n3dsRenderThreadStart())return 6;n3dsRenderThreadJob(init,c.get());}
  else init(c.get());
  mark("renderer init returned");
  if(!c->okay)return 7;
  auto rec=std::make_unique<t2p::FrameRecorder3ds>();
  if(directEventsTest||directGroupsTest)rec->setDirectRegistry(c->r->directRegistry(),directVerifyTest);
  if(directGroupsTest)rec->setGroupRegistry(c->r->groupRegistry());
  rec->setDirectSnapshots(directSnapshotTest);
  if(preparedShapeTest&&!rec->fifo().loadPreparedShapes(N3DS_ROOT "/ww_shapes.bin")){out("ERROR recorder prepared shapes load failed\n");return 12;}
  auto slots=std::make_unique<Slot[]>(2);
  for(int i=0;i<2;i++)if(!slots[i].snapshot.init(2u<<20))return 8;
  mark("snapshots ready");
  if(rigidClipTest||replayTest)rec->setRetainGeometry(true,16384u<<10);
  rec->setGuestRead([&c](uint32_t a,uint32_t n){return c->mem.live(a,n);});
  rec->fifo().setAuroraMode(true);rec->fifo().setPrimitiveRuns(true);
  rec->fifo().setCachedVertexPlan(true);rec->fifo().setFastAttributeDecode(true);rec->fifo().setGeometryCache(false);
  rec->setStateNotes(false);rec->setStateInPlace(true);rec->setStateIncremental(stateIncTest);rec->setStateTemplates(stateTemplatesTest);rec->setCompactDrawStorage(compactTest);rec->setStatePool(poolTest);rec->setVertexPoolFifo(fifoPoolTest);
  if(threaded && heapTest)rec->setHeapProbe(replayHeapRoom);
  unsigned slot=0;rec->select(&slots[slot],size_t(mode)<<20);
  out("START capture %s mode %d passes %d heap %u KB\n",name.c_str(),mode,passes,unsigned(mallinfo().fordblks>>10));
  uint64_t wallStart=0;unsigned wallFrames=0;   // (Claude) threaded cost: wall time of the measured passes
  for(int pass=0;pass<passes;pass++){
    if(pass==1){if(threaded)n3dsGpuSync();wallStart=svcGetSystemTick();}
    at=16;unsigned frame=0;
    for(uint32_t i=0;i<chunks;i++){
      if(!aptMainLoop())return 9;
      uint8_t type=stream[at];at+=4;
      if(type==0){uint32_t n=word(stream,at);auto& fifo=threaded?rec->fifo():c->r->fifo();
        const bool pprof=costTest&&!plainTest&&(!threaded||recProfTest)&&pass>0;uint64_t keep[16];if(pprof){std::memcpy(keep,g_t2pProf,sizeof keep);for(auto& v:g_t2pProf)v=0;g_t2pProfOn=true;}
        const uint64_t pt0=svcGetSystemTick();
        if(t2p::typedReplayProcess(fifo,stream.data()+at,n,threaded && typedTest,c->typedCounts,stateProgramTest)!=n){out("ERROR FIFO short\n");return 10;}at+=n;if(costTest&&(!threaded||recProfTest)&&pass>0)c->processTicks+=svcGetSystemTick()-pt0;
        if(pprof){g_t2pProfOn=false;for(int i=0;i<16;++i){c->pcost[i]+=g_t2pProf[i];g_t2pProf[i]=keep[i];}}}
      else if(type==1){uint32_t a=word(stream,at),n=word(stream,at);
        if(memcmp(c->mem.bytes.data()+a,stream.data()+at,n)){
          memcpy(c->mem.bytes.data()+a,stream.data()+at,n);
          if(threaded)slots[slot].dirty.emplace_back(a,n);else c->r->invalidateTextures(a,n);
        }at+=n;
      }else if(type==2){
        if(pass>0)++wallFrames;
        word(stream,at);
        if(threaded){
          const uint64_t fz0=svcGetSystemTick();
          slots[slot].frozen=slots[slot].snapshot.freeze([&c](uint32_t a,uint32_t n){return c->mem.live(a,n);});
          const uint64_t fz1=svcGetSystemTick();
          n3dsGpuSync(); // mailbox must remain untouched until its previous job finishes
          if(costTest&&pass>0){c->freezeTicks+=fz1-fz0;c->syncTicks+=svcGetSystemTick()-fz1;}
          c->submitted=&slots[slot];c->pass=pass;c->frame=frame;
          n3dsRenderThreadJob(render,c.get()); // returns early only when immutable data is installed
          if(costTest&&pass>0)c->stagingTicks+=slots[slot].stagingWaitTicks;
          slot^=1;rec->select(&slots[slot],size_t(mode)<<20);
        }else{c->pass=pass;c->frame=frame;render(c.get());}
        ++frame;
      }
    }
    if(threaded)n3dsGpuSync();
  }
  if(costTest){
    const double k=c->costFrames?1000.0/double(SYSCLOCK_ARM11)/c->costFrames:0;
    out("COST capture %s mode %d frames %u render %.4f process %.4f ms",name.c_str(),mode,c->costFrames,c->renderTicks*k,c->processTicks*k);
    for(int i=8;i<16;++i)out(" slot%d %.4f",i,c->cost[i]*k);
    out("\nRCOST capture %s (render-side slots 0..7)",name.c_str());for(int i=0;i<8;++i)out(" r%d %.4f",i,c->cost[i]*k);
    out("\nPCOST capture %s (process-side g_t2pProf; slots 2/6 are counts)",name.c_str());
    for(int i=0;i<8;++i)out(" p%d %.4f",i,(i==2||i==6)?double(c->pcost[i])/c->costFrames:c->pcost[i]*k);
    out("\n");
    if(threaded&&recProfTest&&wallFrames){const double w=1000.0/double(SYSCLOCK_ARM11)/wallFrames;
      out("RECPROC capture %s mode %d freeze %.4f handoff-sync %.4f process %.4f staging-wait %.4f work %.4f ms/frame;",name.c_str(),mode,c->freezeTicks*w,c->syncTicks*w,c->processTicks*w,c->stagingTicks*w,(double(c->processTicks)-double(c->stagingTicks))*w);
      for(int i=0;i<16;++i)out(" p%d %.4f",i,(i==2||i==6||i==11)?double(c->pcost[i])/wallFrames:c->pcost[i]*w);out("\n");}
  }
  if(costTest&&threaded&&wallFrames){n3dsGpuSync();out("WALL capture %s mode %d frames %u %.4f ms/frame\n",name.c_str(),mode,wallFrames,double(svcGetSystemTick()-wallStart)*1000.0/double(SYSCLOCK_ARM11)/wallFrames);}
  out("TYPED capture %s mode %d BP %llu CP %llu XF %llu words %llu raw %llu unknown %llu\n",name.c_str(),mode,(unsigned long long)c->typedCounts.bp,(unsigned long long)c->typedCounts.cp,(unsigned long long)c->typedCounts.xf,(unsigned long long)c->typedCounts.words,(unsigned long long)c->typedCounts.raw,(unsigned long long)c->typedCounts.unknown);
  out("SHAPEPROGRAM capture %s mode %d spans %llu hits %llu runs %llu bytes %u\n",name.c_str(),mode,(unsigned long long)c->typedCounts.shapeSpans,(unsigned long long)rec->fifo().preparedShapeHits(),(unsigned long long)rec->fifo().preparedShapeRuns(),unsigned(rec->fifo().preparedShapeBytes()));
  out("MATERIALREF capture %s mode %d references %llu\n",name.c_str(),mode,(unsigned long long)c->typedCounts.materialRefs);
  if(c->typedCounts.unknown)++c->errors;
  out("HEAP capture %s mode %d probe %d checks %u fences %u\n",name.c_str(),mode,int(heapTest),c->heapChecks,c->fences);
  if(textureStoreTest){const auto a=c->r->texPackStats();const auto b=c->r->textureStoreStats();out("STORECHECK capture %s hits %llu verified %llu bad %llu fail %llu reads %llu bytes %llu opens %llu cachehits %llu\n",name.c_str(),(unsigned long long)a.packHits,(unsigned long long)a.packVerified,(unsigned long long)a.packVerifyBad,(unsigned long long)a.packReadFail,(unsigned long long)b.reads,(unsigned long long)b.bytes,(unsigned long long)b.opens,(unsigned long long)b.hits);if(!a.packVerified||a.packVerifyBad||a.packReadFail)++c->errors;}
  out("CLIPCHECK capture %s tests %llu rejected %llu\n",name.c_str(),(unsigned long long)c->clipTests,(unsigned long long)c->clipCulled);
  out("BLOCKS capture %s blocks %u draws %u\n",name.c_str(),c->r->backend().commandBlocks(),c->r->backend().commandBlockDraws());
  out("END capture %s mode %d miss %u verify-miss %u fallback %u errors %u\n",name.c_str(),mode,c->mem.misses,c->mem.verifyMisses,c->fallbacks,c->errors);
  if(threaded){n3dsRenderThreadJob(fini,c.get());n3dsGpuShutdown();}else fini(c.get());
  return c->errors||c->mem.misses||c->fallbacks?11:0;
}
bool requested(){
  auto cfg=load(std::string(dir)+"config.txt");
  const std::string s(cfg.begin(),cfg.end());
  return s.find("phase chatgpt-core2-parity-v1")!=std::string::npos;
}
int run(){
  auto cfg=load(std::string(dir)+"config.txt");std::string config(cfg.begin(),cfg.end());
  // (Claude v63) every "phase ..." line is a separate option set, run in order; flags are read from that line only
  auto parse=[](const std::string& config){copyReuseTest=config.find(" copyreuse=1")!=std::string::npos;directGroupColdTest=config.find(" groupcold=1")!=std::string::npos;directSnapshotTest=config.find(" directsnapshot=1")!=std::string::npos;directGroupsTest=config.find(" directgroups=1")!=std::string::npos;stateTemplatesTest=config.find(" templates=1")!=std::string::npos;t2p::g_stateCpIncremental=config.find(" cpinc=1")!=std::string::npos;t2p::g_uniformPieces=config.find(" upieces=1")!=std::string::npos;directSlotsTest=config.find(" directslots=512")!=std::string::npos?512:config.find(" directslots=256")!=std::string::npos?256:128;directVerifyTest=config.find(" directverify=1")!=std::string::npos;t2p::g_picaForceZDepth=config.find(" zdepth=1")!=std::string::npos;directEventsTest=config.find(" directevents=1")!=std::string::npos;preparedShapeTest=config.find(" shapes=1")!=std::string::npos;t2p::shapeSpansReplay=config.find(" spans=1")!=std::string::npos;preparedDrawTest=config.find(" prepared=1")!=std::string::npos;t2p::materialRefsReplay=config.find(" materialrefs=1")!=std::string::npos;kick48Test=config.find(" kick48=1")!=std::string::npos;modelProgramTest=config.find(" modelprog=1")!=std::string::npos;t2p::g_sharedMaterialVectors=config.find(" sharedmat=1")!=std::string::npos;uniformDeltaTest=config.find(" udelta=1")!=std::string::npos;replayTest=config.find(" replay=1")!=std::string::npos;commandBlocksTest=config.find(" blocks=1")!=std::string::npos;modelCommandTest=config.find(" modelcmd=1")!=std::string::npos;textureStoreTest=config.find(" texstore=1")!=std::string::npos;rigidClipTest=config.find(" rigidclip=1")!=std::string::npos;t2p::g_compactPoseStorage=config.find(" compactpose=1")!=std::string::npos;t2p::g_uniformSections=config.find(" sections=1")!=std::string::npos;stateProgramTest=config.find(" programs=1")!=std::string::npos;t2p::g_packVertexMajor=config.find(" vmajor=1")!=std::string::npos;t2p::g_packPlanar=config.find(" planar=1")!=std::string::npos;stateIncTest=config.find(" stateinc=1")!=std::string::npos;compactTest=config.find(" compact=1")!=std::string::npos;reuseTest=config.find(" reuse=1")!=std::string::npos;texErrTest=config.find(" texerror=1")!=std::string::npos;packetWorkerTest=config.find(" packetworker=1")!=std::string::npos;t2p::g_packetWorkerCore=config.find(" pwcore=0")!=std::string::npos?0:1;packetWorkerVerifyTest=config.find(" packetverify=1")!=std::string::npos;bindingMemoTest=config.find(" bindmemo=0")==std::string::npos;planTemplateTest=config.find(" plantemplates=1")!=std::string::npos;matrixCostTest=config.find(" matrixcost=1")!=std::string::npos;splitTest=config.find(" split=1")!=std::string::npos;plainTest=config.find(" plain=1")!=std::string::npos;recProfTest=config.find(" recproc=1")!=std::string::npos;matrixBatchTest=config.find(" matrixbatch=1")!=std::string::npos;depthTest=config.find(" depth=1")!=std::string::npos;{size_t at=config.find(" palette=");paletteTest=at==std::string::npos?0:atoi(config.c_str()+at+9);if(paletteTest!=2 && paletteTest!=10)paletteTest=0;}t2p::g_decodedInitMode=config.find(" overwrite=1")!=std::string::npos?1:0;directPackTest=config.find(" directpack=1")!=std::string::npos;quietPlanTest=config.find(" quietplan=1")!=std::string::npos;costTest=config.find(" costs=1")!=std::string::npos;fifoPoolTest=config.find(" vpool=1")!=std::string::npos;poolTest=config.find(" pool=1")!=std::string::npos;directTest=config.find(" direct=1")!=std::string::npos;heapTest=config.find(" heap=1")!=std::string::npos;typedTest=config.find(" typed=1")!=std::string::npos;};

  std::vector<std::string> phases;{size_t a=0;while(a<config.size()){size_t e=config.find('\n',a);if(e==std::string::npos)e=config.size();std::string l=config.substr(a,e-a);if(l.rfind("phase ",0)==0)phases.push_back(l+" ");a=e+1;}}
  if(phases.empty())phases.push_back(config);
  remove((std::string(dir)+"config.txt").c_str()); // one-shot test entry cannot hijack a later normal boot
  JKRPcBeginHostAlloc();
  log=fopen((std::string(dir)+"results.txt").c_str(),"w");if(!log){JKRPcEndHostAlloc();return 1;}
  int warm=1,measured=1;size_t p=config.find("passes ");if(p!=std::string::npos)sscanf(config.c_str()+p,"passes %d %d",&warm,&measured);
  if(config.find(" fastalloc=1")!=std::string::npos){::n3dsFastAllocEnable(true);out("FASTALLOC on (whole run)\n");}   // (Claude v77) as the game (fast_alloc=1); one-way: every phase after it
  auto bundle=load(std::string(dir)+"bundle.bin");int errors=0,executed=0;
  for(const std::string& ph:phases){
    parse(ph);out("PHASE %s\n",ph.c_str());
  // The test input is game-derived and temporary, never embedded in the CIA.
  if(t2p::shapeSpansReplay && t2p::shapeReplaySpans.empty()){
   auto input=load(N3DS_ROOT "/ww_shape_spans.bin");size_t at=0;
   if(input.size()<12||std::memcmp(input.data(),"WWSS",4)){out("ERROR shape span header\n");return 20;}
   at=4;if(word(input,at)!=1){out("ERROR shape span version\n");return 20;}uint32_t count=word(input,at);
   if(count>128){out("ERROR shape span count\n");return 20;}
   for(uint32_t i=0;i<count;++i){if(input.size()-at<4)return 20;uint32_t n=word(input,at);if(n>262144||n>input.size()-at)return 20;t2p::shapeReplaySpans.emplace_back(input.begin()+at,input.begin()+at+n);at+=n;}
   if(at!=input.size())return 20;
  }

    for(const char* cap:{"cliff","aerial","windfall_title_v25"}){   // one capture in memory at a time (console heap)
    if(config.find(std::string("capture ")+cap)==std::string::npos)continue;
    mark("loading capture");
    auto stream=load(std::string(dir)+cap+".wwrp");
    mark("capture loaded");
    if(stream.size()<16||memcmp(stream.data(),"WWRP",4)){out("ERROR capture %s unreadable\n",cap);++errors;continue;}
    for(int mode:{0,2,8}){size_t mp=ph.find(" mode=");if(mp!=std::string::npos && mode!=atoi(ph.c_str()+mp+6))continue;++executed;int e=phase(cap,stream,bundle,mode,std::max(1,std::min(4,warm+measured)));
      if(e){out("ERROR capture %s mode %d code %d\n",cap,mode,e);++errors;break;}}
    }
  }
  if(!executed){out("ERROR no supported phase executed\n");++errors;}
  if(modelCommandTest){const int failures=n3dsModelCommandTest();out("MODELCMD cases 300 errors %d\n",failures);errors+=failures;}
  out("DONE errors %d\n",errors);fclose(log);JKRPcEndHostAlloc();gfxExit();return errors;
}
} // namespace core2test
