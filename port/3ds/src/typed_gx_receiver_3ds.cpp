#include "typed_gx_api_3ds.h"
#include "n3ds.h"
#include "performance_3ds.h"
#include "gxfifo.h"
namespace {
t2p::GxFifo* receiver=nullptr;
bool ready=false,requested=false;
unsigned frames=0,bp=0,cp=0,xf=0,words=0;
}
extern "C" {
int g_n3dsTypedFrame=0;
int g_n3dsTypedPrimitive=0;
uint32_t g_n3dsTypedConsumedSize=0;
void n3dsTypedRegisterFifo(void* fifo) {
  Performance3ds config;
  if(FILE* f=std::fopen(N3DS_ROOT "/performance.ini","r")){config.read(f);std::fclose(f);}
  receiver=static_cast<t2p::GxFifo*>(fifo);
  requested=config.typedGx;
  ready=requested && !config.fifoThread && !config.retainGeometry && !config.geometryCache;
  n3dsLog("[typed-gx] requested %d effective %d inline %d retained %d geometry_cache %d ignored %u\n",
    int(requested),int(ready),!config.fifoThread,int(config.retainGeometry),int(config.geometryCache),config.errors);
}
void n3dsTypedUnregisterFifo(void* fifo) {
  if(receiver==fifo){receiver=nullptr;ready=false;g_n3dsTypedFrame=0;}
}
void n3dsTypedBeginFrame(){g_n3dsTypedFrame=ready && receiver;g_n3dsTypedPrimitive=0;}
void n3dsTypedEndFrame(){
  g_n3dsTypedFrame=0;
  if(requested && ++frames%60==0){
    n3dsLog("[typed-gx] frame %u: BP %u CP %u XF %u words %u (60-frame totals)\n",frames,bp,cp,xf,words);
    bp=cp=xf=words=0;
  }
}
void n3dsTypedApplyBp(uint32_t v){receiver->applyBp(v);++bp;}
void n3dsTypedApplyCp(uint8_t a,uint32_t v){receiver->applyCp(a,v);++cp;}
void n3dsTypedApplyXf(uint32_t a,const uint8_t* be,uint32_t n){receiver->applyXf(a,be,n);++xf;words+=n;}
}
