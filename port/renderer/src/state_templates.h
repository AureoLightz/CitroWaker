#pragma once
#include "gxretain.h"
#include "direct_draw_binding.h"
namespace t2p {
// Prototype: reuse only decoded STATIC state. No borrowed guest addresses or
// pose snapshots survive in these templates. Dynamic XF inputs are rebuilt.
class StateTemplateCache {
 struct Entry {
  DrawRegisterSignature signature;
  GxState state;
  unsigned age;
  Entry(const GxRegs& r,const GxState& s,unsigned a):signature(r,true),state(s),age(a){state.uniformBlock.reset();state.uniformSections.reset();}
 };
 std::array<std::unique_ptr<Entry>,256> slots{};
 unsigned clock=0;
 static unsigned bucket(const GxRegs& r){unsigned k=r.texAddr[0]^(r.bp[0xc0]*0x9e3779b1u)^r.bp[0x00];k^=k>>16;return (k&63)*4;}
public:
 unsigned hits=0,misses=0;
 const GxState* find(const GxRegs& r){const auto first=bucket(r);for(unsigned i=first;i<first+4;++i)if(slots[i]&&slots[i]->signature.matches(r)){slots[i]->age=++clock;++hits;return &slots[i]->state;}++misses;return nullptr;}
 void remember(const GxRegs& r,const GxState& s){const auto first=bucket(r);unsigned victim=first;for(unsigned i=first;i<first+4;++i){if(!slots[i]){victim=i;break;}if(slots[i]->age<slots[victim]->age)victim=i;}slots[victim]=std::make_unique<Entry>(r,s,++clock);}
 static constexpr uint32_t dynamic=kStateDirtyXfMtx|kStateDirtyLight|kStateDirtyChan|kStateDirtyProj|kStateDirtyFog;
};
}
