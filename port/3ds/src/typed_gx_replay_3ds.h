#pragma once
// Test adapter only: replay recorded register packets through the same public
// apply* contract, with all other packets still consumed by the canonical FIFO.
#include "gxfifo.h"
extern "C" uint64_t n3dsMaterialRegister(const uint8_t*,uint32_t);
namespace t2p {
inline bool shapeSpansReplay=false;
inline std::vector<std::vector<uint8_t>> shapeReplaySpans;
inline bool materialRefsReplay=false;
struct TypedReplayCounts {uint64_t shapeSpans=0,materialRefs=0,bp=0,cp=0,xf=0,words=0,raw=0,unknown=0;};
inline size_t typedReplayProcess(GxFifo& f,const uint8_t* data,size_t size,bool apply,TypedReplayCounts& counts,bool programs=false){
 if(!apply && !programs && !materialRefsReplay && !shapeSpansReplay)return f.process(data,size);
 auto be16=[](const uint8_t*p){return uint32_t(p[0])<<8|p[1];};
 auto be32=[](const uint8_t*p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];};
 size_t at=0;
 while(at<size){
  const uint8_t* p=data+at;const size_t left=size-at;
  const unsigned cmd=p[0],op=cmd&0xf8;size_t n=0;
  if(shapeSpansReplay && cmd>=0x80){bool matched=false;
   for(const auto& span:shapeReplaySpans)if(span.size()<=left && span.size()>=16 && !std::memcmp(span.data(),p,16) && !std::memcmp(span.data(),p,span.size())){
    f.processShapeRef(p,uint32_t(span.size()),1);at+=span.size();++counts.shapeSpans;matched=true;break;
   }
   if(matched)continue;
  }

  if(materialRefsReplay){
   // Qualification adapter: feed complete pure register runs through the SAME
   // owned registry/token decoder as the producer. No draw or side effect moves.
   size_t run=0;
   while(run<left){const uint8_t* q=p+run;size_t remain=left-run,n=0;
    if(q[0]==0)n=1;
    else if(q[0]==8 && remain>=6)n=6;
    else if(q[0]==0x61 && remain>=5 && q[1]!=0x45 && q[1]!=0x52 && q[1]!=0x65)n=5;
    else if(q[0]==0x10 && remain>=5){n=5+4*size_t((be32(q+1)>>16)+1);if(n>remain)n=0;}
    if(!n || run+n>2048)break;run+=n;
   }
   if(run){const uint64_t token=n3dsMaterialRegister(p,uint32_t(run));if(token){
     uint8_t packet[11]={0x50,0,0x7a};for(unsigned i=0;i<8;++i)packet[3+i]=uint8_t(token>>(56-8*i));
     if(f.process(packet,sizeof packet)!=sizeof packet)return at;
     ++counts.materialRefs;at+=run;continue;
   }}
  }

  if(cmd==0)n=1;
  else if(cmd>=0x80 && left>=3){
   n=3+size_t(be16(p+1))*gxVertexSize(f.regs().vcd(),f.regs().vat(cmd&7));
  }else if(op==8 && left>=6){
   if(!programs || !f.processStateProgram(p,6))f.applyCp(p[1],be32(p+2));++counts.cp;at+=6;continue;
  }else if(op==0x10 && left>=5){
   const uint32_t h=be32(p+1),num=(h>>16)+1;
   n=5+size_t(num)*4;
   if(n<=left){if(!programs || !f.processStateProgram(p,n))f.applyXf(h&0xffff,p+5,num);++counts.xf;counts.words+=num;at+=n;continue;}
  }else if(cmd==0x61 && left>=5){
   const uint32_t v=be32(p+1);const auto id=v>>24;
   if(id!=0x45 && id!=0x52 && id!=0x65){if(!programs || !f.processStateProgram(p,5))f.applyBp(v);++counts.bp;at+=5;continue;}
   n=5;
  }else if(op>=0x20 && op<=0x38)n=5;
  else if(op==0x40)n=9;
  else if(op==0x48)n=1;
  else if(cmd==0x50 && left>=3){
   const unsigned sub=be16(p+1);size_t payload=~size_t(0);
   if(sub>=0x10 && sub<=0x1f)payload=13;
   else switch(sub){
    case 1:payload=24;break;case 2:payload=16;break;case 3:payload=64;break;
    case 0x20:case 0x22:if(left>=5)payload=2+be16(p+3);break;
    case 0x21:case 0x38:case 0x3a:payload=0;break;
    case 0x30:payload=34;break;case 0x31:payload=23;break;
    case 0x32:case 0x33:case 0x50:payload=4;break;
    case 0x34:case 0x37:case 0x39:payload=8;break;
    case 0x35:payload=16;break;case 0x36:payload=13;break;
    case 0x40:if(left>=8)payload=5+be32(p+4);break;
    case 0x51:if(left>=4)payload=p[3]?97:1;break;
    case 0x7e:if(left>=7)payload=4+be32(p+3);break;
    case 0x1000:payload=20;break;
   }
   if(payload!=~size_t(0))n=3+payload;
  }
  if(!n || n>left){++counts.unknown;return at+f.process(p,left);}
  const size_t got=f.process(p,n);++counts.raw;at+=got;
  if(got!=n)return at;
 }
 return at;
}
}
