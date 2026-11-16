#include "gxdecodecache.h"
#include "gxfifo.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace t2p {
namespace {
uint32_t hashWords(const void* ptr,size_t n,uint32_t h){
 const auto* p=static_cast<const uint8_t*>(ptr);
 while(n>=4){uint32_t v;std::memcpy(&v,p,4);h=(h^v)*0x9e3779b1u;p+=4;n-=4;}
 while(n--){h=(h^*p++)*0x01000193u;}return h;
}
bool indexed(const GxAttrPlan&a){return a.kind!=GxAttrPlan::Byte && a.type!=GXA_DIRECT;}
uint32_t indexAt(const GxAttrPlan&a,const uint8_t*p){return a.type==GXA_INDEX16?(uint32_t(p[0])<<8)|p[1]:*p;}
bool safeFloat(const uint8_t*p,bool be){
 uint32_t v;if(be)v=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];else std::memcpy(&v,p,4);
 const auto exp=v&0x7f800000u;
 // Non-finite/subnormal inputs keep their original FPU exception/FTZ behavior.
 return exp!=0x7f800000u && (exp!=0 || (v&0x007fffffu)==0);
}
}
void GxDecodeCache::erase(Iterator it){
 auto r=byHash_.equal_range(it->hash);
 for(auto p=r.first;p!=r.second;++p)if(p->second==it){byHash_.erase(p);break;}
 used_-=it->cost;entries_.erase(it);
}
bool GxDecodeCache::lookup(const uint32_t format[5],const uint8_t* stream,uint32_t bytes,
 const GxAttrPlan* attrs,int count,uint32_t vertices,uint32_t,uint64_t clock){
 pending_=false;clock_=clock;++stats_.lookups;stats_.vertices+=vertices;
 if(vertices<12 || vertices>512 || count<=0 || uint64_t(count)*vertices>4096 || bytes>65536){++stats_.bypassed;return false;}
 uint32_t h=hashWords(format,20,0x811c9dc5u);h=hashWords(&vertices,4,h);h=hashWords(stream,bytes,h);
 for(int k=0;k<count;k++)if(indexed(attrs[k])){
  const auto&a=attrs[k];if(!a.base){++stats_.bypassed;return false;}
  uintptr_t ptr=reinterpret_cast<uintptr_t>(a.base);h=hashWords(&ptr,sizeof ptr,h);
  h=hashWords(&a.stride,4,h);h=hashWords(&a.size,4,h);h=hashWords(&a.be,sizeof a.be,h);
 }
 pendingHash_=h;
 auto r=byHash_.equal_range(h);
 for(auto p=r.first;p!=r.second;++p){
  auto it=p->second;auto&e=*it;
  if(e.vertices!=vertices || e.bindings.size()!=size_t(count) || e.stream.size()!=bytes || std::memcmp(e.format,format,20) || std::memcmp(e.stream.data(),stream,bytes))continue;
  bool same=true;
  for(int k=0;k<count&&same;k++)if(indexed(attrs[k])){
   const auto&a=attrs[k];const auto&b=e.bindings[k];same=a.base==b.base&&a.stride==b.stride&&a.size==b.size&&a.be==b.be;
  }
  if(!same)continue;
  for(const auto&s:e.spans)if(std::memcmp(s.address,e.source.data()+s.offset,s.bytes)){same=false;break;}
  if(!same){
   ++stats_.changed;cooldown_[h&1023]={h,clock+100000};erase(it);return false;
  }
  size_t offset=0;
  for(int k=0;k<count;k++){size_t n=size_t(vertices)*attrs[k].outStride;std::memcpy(attrs[k].out,e.decoded.data()+offset,n);offset+=n;}
  e.lastUse=clock;entries_.splice(entries_.begin(),entries_,it);++stats_.hits;stats_.hitVertices+=vertices;return true;
 }
 const auto& cool=cooldown_[h&1023];if(cool.hash==h&&clock<cool.until){++stats_.bypassed;return false;}
 pending_=true;return false;
}
void GxDecodeCache::store(const uint32_t format[5],const uint8_t* stream,uint32_t bytes,
 const GxAttrPlan* attrs,int count,uint32_t vertices,uint32_t vsize){
 if(!pending_)return;pending_=false;
 Entry e;e.hash=pendingHash_;e.vertices=vertices;e.lastUse=clock_;std::memcpy(e.format,format,20);
 std::vector<Span> refs;refs.reserve(size_t(count)*vertices);
 size_t decoded=0;
 for(int k=0;k<count;k++){
  const auto&a=attrs[k];decoded+=size_t(vertices)*a.outStride;
  if(a.kind!=GxAttrPlan::Byte && a.kind!=GxAttrPlan::Clr && a.fmt>GXC_F32)return;
  for(uint32_t v=0;v<vertices;v++){
   const uint8_t* p=stream+size_t(v)*vsize+a.offset;
   if(indexed(a)){
    uint64_t off=uint64_t(indexAt(a,p))*a.stride;if(off+a.need>a.size)return;
    p=a.base+off;refs.push_back({p,a.need,0});
   }
   if(a.kind!=GxAttrPlan::Byte&&a.kind!=GxAttrPlan::Clr&&a.fmt==GXC_F32){
    for(int c=0;c<a.nc;c++)if(!safeFloat(p+4*c,a.type==GXA_DIRECT||a.be))return;
   }
  }
 }
 // Merge only referenced overlapping/adjacent bytes. Never read padding or
 // holes merely because the first and last indices happen to be far apart.
 std::sort(refs.begin(),refs.end(),[](const Span&a,const Span&b){return reinterpret_cast<uintptr_t>(a.address)<reinterpret_cast<uintptr_t>(b.address);});
 for(const auto&s:refs){
  uintptr_t start=reinterpret_cast<uintptr_t>(s.address),end=start+s.bytes;
  if(!e.spans.empty()){
   auto& prev=e.spans.back();uintptr_t ps=reinterpret_cast<uintptr_t>(prev.address),pe=ps+prev.bytes;
   if(start<=pe){if(end>pe)prev.bytes=uint32_t(end-ps);continue;}
  }
  e.spans.push_back(s);
 }
 if(e.spans.size()>128)return;
 size_t source=0;for(auto&s:e.spans){s.offset=uint32_t(source);source+=s.bytes;}
 if(source>65536 || decoded>65536)return;
 const size_t estimate=sizeof(Entry)+96+bytes+source+decoded+size_t(count)*sizeof(Binding)+e.spans.capacity()*sizeof(Span);
 if(estimate>budget_)return;
 // Protect a resident working subset instead of thrashing when a full frame
 // contains more geometry than fits. Changed sources are evicted immediately.
 while(!entries_.empty()&&(used_+estimate>budget_||entries_.size()>=1024)){
  auto tail=std::prev(entries_.end());if(clock_-tail->lastUse<15000)return;erase(tail);
 }
 if(used_+estimate>budget_)return;
 e.stream.assign(stream,stream+bytes);e.bindings.resize(count);e.source.resize(source);e.decoded.resize(decoded);
 for(const auto&s:e.spans)std::memcpy(e.source.data()+s.offset,s.address,s.bytes);
 size_t offset=0;
 for(int k=0;k<count;k++){
  const auto&a=attrs[k];if(indexed(a))e.bindings[k]={a.base,a.stride,a.size,a.be};
  size_t n=size_t(vertices)*a.outStride;std::memcpy(e.decoded.data()+offset,a.out,n);offset+=n;
 }
 e.cost=sizeof(Entry)+96+e.stream.capacity()+e.source.capacity()+e.decoded.capacity()+e.bindings.capacity()*sizeof(Binding)+e.spans.capacity()*sizeof(Span);
 if(used_+e.cost>budget_)return;
 used_+=e.cost;entries_.push_front(std::move(e));byHash_.emplace(entries_.front().hash,entries_.begin());++stats_.admitted;
}
} // namespace t2p
