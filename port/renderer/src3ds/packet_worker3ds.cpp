#include "packet_worker3ds.h"
#include <3ds.h>
#include <cassert>
#include <array>
void JKRPcBeginHostAlloc() __attribute__((weak));
void JKRPcEndHostAlloc() __attribute__((weak));
namespace t2p {
struct PacketWorker3ds::Impl {
 enum State {Free,Queued,Building,Ready,Held};
 struct Slot {
  State state=Free;
  std::array<Input,BatchSize> input{};size_t count=0;
  Options options;FrameConfig frame;DrawCompileCache* cache=nullptr;bool notes=false;
  u32 fpscr=0;std::array<DrawPacket,BatchSize> packet;
 } slots[2];
 LightLock lock;LightEvent work,ready;
 Thread thread=nullptr;bool stopping=false;
 unsigned submitted=0,built=0,consumed=0;size_t offset=0;uint64_t waits=0;
 Impl(){LightLock_Init(&lock);LightEvent_Init(&work,RESET_ONESHOT);LightEvent_Init(&ready,RESET_ONESHOT);}
 static void entry(void* p){if(JKRPcBeginHostAlloc)JKRPcBeginHostAlloc();static_cast<Impl*>(p)->run();if(JKRPcEndHostAlloc)JKRPcEndHostAlloc();}
 void run(){
  for(;;){
   LightLock_Lock(&lock);
   if(stopping){LightLock_Unlock(&lock);return;}
   Slot& s=slots[built&1];
   if(s.state!=Queued){LightLock_Unlock(&lock);LightEvent_Wait(&work);continue;}
   s.state=Building;LightLock_Unlock(&lock);
#ifdef __arm__
   asm volatile("vmsr fpscr, %0"::"r"(s.fpscr):"memory");
#endif
   for(size_t n=0;n<s.count;++n)buildDrawInto(s.packet[n],*s.input[n].st,*s.input[n].draw,s.options,s.frame,s.cache,s.notes);
   LightLock_Lock(&lock);s.state=Ready;++built;LightLock_Unlock(&lock);LightEvent_Signal(&ready);
  }
 }
};
PacketWorker3ds::PacketWorker3ds():impl_(std::make_unique<Impl>()){}
PacketWorker3ds::~PacketWorker3ds(){stop();}
int g_packetWorkerCore=1;   // (Claude) 1: system core (80% app limit), as before; 0: the game core at the lowest app priority (uses its idle time)
bool PacketWorker3ds::start(const std::function<void(const std::string&)>& log){
 if(impl_->thread)return true;
 if(g_packetWorkerCore==0) impl_->thread=threadCreate(Impl::entry,impl_.get(),512u<<10,0x3E,0,false);
 else { Result limit=APT_SetAppCpuTimeLimit(80);
 if(R_SUCCEEDED(limit))impl_->thread=threadCreate(Impl::entry,impl_.get(),512u<<10,0x30,1,false); }
 if(log&&!impl_->thread)log("packet-worker unavailable; render-owner packet building retained");
 return impl_->thread!=nullptr;
}
void PacketWorker3ds::stop(){
 if(!impl_||!impl_->thread)return;
 LightLock_Lock(&impl_->lock);impl_->stopping=true;LightLock_Unlock(&impl_->lock);LightEvent_Signal(&impl_->work);
 threadJoin(impl_->thread,U64_MAX);threadFree(impl_->thread);impl_->thread=nullptr;
}
bool PacketWorker3ds::active()const{return impl_->thread!=nullptr;}
void PacketWorker3ds::submit(const GxState& st,const GxDraw& d,const Options& o,const FrameConfig& f,DrawCompileCache* c,bool notes){
 const Input input{&st,&d};submitBatch(&input,1,o,f,c,notes);
}
void PacketWorker3ds::submitBatch(const Input* input,size_t count,const Options& o,const FrameConfig& f,DrawCompileCache* c,bool notes){
 assert(count&&count<=BatchSize);
 auto& i=*impl_;LightLock_Lock(&i.lock);auto& s=i.slots[i.submitted&1];assert(s.state==Impl::Free);
#ifdef __arm__
 asm volatile("vmrs %0, fpscr":"=r"(s.fpscr)::"memory");
#endif
 std::copy_n(input,count,s.input.begin());s.count=count;s.options=o;s.frame=f;s.cache=c;s.notes=notes;s.state=Impl::Queued;++i.submitted;
 LightLock_Unlock(&i.lock);LightEvent_Signal(&i.work);
}
const DrawPacket& PacketWorker3ds::acquire(bool timed){
 auto& i=*impl_;uint64_t start=0;
 for(;;){LightLock_Lock(&i.lock);auto& s=i.slots[i.consumed&1];
  if(s.state==Impl::Ready||s.state==Impl::Held){s.state=Impl::Held;LightLock_Unlock(&i.lock);if(start)i.waits+=svcGetSystemTick()-start;return s.packet[i.offset];}
  LightLock_Unlock(&i.lock);if(timed&&!start)start=svcGetSystemTick();LightEvent_Wait(&i.ready);
 }
}
void PacketWorker3ds::release(){
 auto& i=*impl_;LightLock_Lock(&i.lock);auto& s=i.slots[i.consumed&1];assert(s.state==Impl::Held);
 // Keep the batch-owned upload buffers bounded even as model sizes vary.
 if(++i.offset==s.count){
  size_t bytes=0;for(const auto& p:s.packet)bytes+=p.vertices.capacity()+p.indices.capacity()*sizeof(uint16_t);
  if(bytes>(512u<<10))for(auto& p:s.packet)p=DrawPacket();
  s.count=0;s.state=Impl::Free;++i.consumed;i.offset=0;
 }
 LightLock_Unlock(&i.lock);
}
void PacketWorker3ds::resetStats(){impl_->waits=0;}
uint64_t PacketWorker3ds::waitTicks()const{return impl_->waits;}
}
