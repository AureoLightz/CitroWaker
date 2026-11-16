#pragma once
#include <algorithm>
#include <vector>
#include <cstring>
extern "C" void C3Dx_Uniforms(const u8*,const float (*)[4],int);
// Run only after final renderer shutdown: the reference mutates private Citro3D uniform history.
inline int n3dsModelCommandTest(){
 using Row=t2p::Backend3ds::UniformRowWords;
 std::vector<uint8_t> uniforms(sizeof C3D_FVUnif),dirty(sizeof C3D_FVUnifDirty);
 std::memcpy(uniforms.data(),C3D_FVUnif,uniforms.size());std::memcpy(dirty.data(),C3D_FVUnifDirty,dirty.size());
 u32* old=nullptr;u32 oldSize=0,oldAt=0;GPUCMD_GetBuffer(&old,&oldSize,&oldAt);
 u32 live[32];for(auto& x:live)x=0x1234abcd;GPUCMD_SetBuffer(live,32,8);
 uint32_t rng=0x71cab00d;auto random=[&](){rng=rng*1664525u+1013904223u;return rng;};int errors=0;
 for(int test=0;test<300;++test){
  const size_t n=size_t(test%97);std::vector<Row> rows;std::vector<unsigned> regs(96);for(unsigned i=0;i<96;++i)regs[i]=i;
  for(unsigned i=95;i>0;--i)std::swap(regs[i],regs[random()%(i+1)]);
  for(size_t i=0;i<n;++i){Row r;r.reg=uint8_t(regs[i]);for(auto& x:r.xyzw)x=random();rows.push_back(r);}
  std::vector<uint32_t> actual;const bool ok=t2p::Backend3ds::makeUniformCommands(rows.data(),n,actual);
  u32* ptr=nullptr;u32 size=0,at=0;GPUCMD_GetBuffer(&ptr,&size,&at);
  if(!ok||ptr!=live||size!=32||at!=8||std::memcmp(uniforms.data(),C3D_FVUnif,uniforms.size())||std::memcmp(dirty.data(),C3D_FVUnifDirty,dirty.size()))++errors;
  for(auto x:live)if(x!=0x1234abcd){++errors;break;}
  std::sort(rows.begin(),rows.end(),[](const Row& a,const Row& b){return a.reg<b.reg;});
  u8 ids[96];float values[96][4];for(size_t i=0;i<n;++i){ids[i]=rows[i].reg;std::memcpy(values[i],rows[i].xyzw,16);}
  std::vector<uint32_t> reference(1024,0);GPUCMD_SetBuffer(reference.data(),1024,0);C3Dx_Uniforms(ids,values,int(n));GPUCMD_GetBuffer(nullptr,nullptr,&at);reference.resize(at);
  if(reference!=actual)++errors;
  std::memcpy(C3D_FVUnif,uniforms.data(),uniforms.size());std::memcpy(C3D_FVUnifDirty,dirty.data(),dirty.size());GPUCMD_SetBuffer(live,32,8);
 }
 Row duplicate[2]{};std::vector<uint32_t> untouched{0xaaaaaaaa};if(t2p::Backend3ds::makeUniformCommands(duplicate,2,untouched)||untouched!=std::vector<uint32_t>{0xaaaaaaaa})++errors;
 Row bad;bad.reg=96;if(t2p::Backend3ds::makeUniformCommands(&bad,1,untouched))++errors;
 GPUCMD_SetBuffer(old,oldSize,oldAt);return errors;
}
