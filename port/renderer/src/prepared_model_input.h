#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <cstring>
namespace t2p {
struct ModelCompilerInput {
 struct Array {uint32_t attribute,count,type,fraction,stride;std::vector<uint8_t> data;};
 struct Group {uint32_t shape,group,material,matrixType,matrixIndex;std::vector<uint32_t> palette;std::vector<std::array<uint32_t,2>> attrs;std::vector<uint8_t> stream;};
 std::array<uint8_t,32> source{};std::vector<Array> arrays;std::vector<std::vector<uint8_t>> materials;std::vector<Group> groups;
 static bool load(const uint8_t* p,size_t n,ModelCompilerInput& output){
  if(!p||n<52||n>32*1024*1024||std::memcmp(p,"WWMI",4))return false;
  size_t at=4;bool ok=true;
  auto u32=[&](){if(n-at<4){ok=false;return uint32_t(0);}uint32_t v=uint32_t(p[at])|uint32_t(p[at+1])<<8|uint32_t(p[at+2])<<16|uint32_t(p[at+3])<<24;at+=4;return v;};
  if(u32()!=1)return false;ModelCompilerInput m;std::memcpy(m.source.data(),p+at,32);at+=32;
  const uint32_t na=u32(),nm=u32(),ng=u32();if(na>12||nm>4096||ng>65536)return false;
  auto blob=[&](std::vector<uint8_t>& b,size_t limit){uint32_t size=u32();if(!ok||size>limit||size>n-at){ok=false;return;}b.assign(p+at,p+at+size);at+=size;};
  for(uint32_t i=0;i<na;++i){Array a;a.attribute=u32();a.count=u32();a.type=u32();a.fraction=u32();a.stride=u32();if(a.attribute<9||a.attribute>20||a.count>1||a.fraction>31||!a.stride||a.stride>16||a.type>((a.attribute==11||a.attribute==12)?5u:4u))return false;for(const auto& old:m.arrays)if(old.attribute==a.attribute)return false;blob(a.data,16*1024*1024);if(!ok)return false;m.arrays.push_back(std::move(a));}
  for(uint32_t i=0;i<nm;++i){std::vector<uint8_t> b;blob(b,256*1024);if(!ok)return false;m.materials.push_back(std::move(b));}
  for(uint32_t i=0;i<ng;++i){Group g;g.shape=u32();g.group=u32();g.material=u32();g.matrixType=u32();g.matrixIndex=u32();if(g.material>=nm||g.matrixType>3)return false;uint32_t np=u32();if(np>10)return false;while(np--)g.palette.push_back(u32());uint32_t nv=u32();if(nv>21)return false;while(nv--){uint32_t a=u32(),t=u32();if(a>20||t>3||(a<9&&t>1))return false;for(auto old:g.attrs)if(old[0]==a)return false;g.attrs.push_back({a,t});}blob(g.stream,4*1024*1024);if(!ok)return false;m.groups.push_back(std::move(g));}
  if(!ok||at!=n)return false;output=std::move(m);return true;
 }
 // The precise CP VAT/VCD words used by the decoder, without pointer bindings.
 bool vertexFormat(const Group& g,std::array<uint32_t,5>& cp)const{
  cp={};for(auto pair:g.attrs){unsigned a=pair[0],t=pair[1];if(a<9)cp[0]|=t<<a;else if(a<=12)cp[0]|=t<<(9+2*(a-9));else cp[1]|=t<<(2*(a-13));}
  cp[2]|=1u<<30;cp[3]|=1u<<31;
  for(unsigned attr=9;attr<=20;++attr){
   Array a{};a.attribute=attr;a.count=attr==10?0:1;a.type=attr==11||attr==12?5:4;a.fraction=0;
   for(const auto& candidate:arrays)if(candidate.attribute==attr){a.count=candidate.count;a.type=candidate.type;a.fraction=candidate.fraction;break;}
   auto c=a.count,t=a.type,f=a.fraction;
   if(a.attribute==9)cp[2]|=c|(t<<1)|(f<<4);
   else if(a.attribute==10){if(c)return false;cp[2]|=t<<10;}
   else if(a.attribute==11)cp[2]|=(c<<13)|(t<<14);
   else if(a.attribute==12)cp[2]|=(c<<17)|(t<<18);
   else {unsigned slot=a.attribute-13;
    if(slot==0)cp[2]|=(c<<21)|(t<<22)|(f<<25);
    else if(slot<=3){unsigned shift=9*(slot-1);cp[3]|=(c<<shift)|(t<<(shift+1))|(f<<(shift+4));}
    else if(slot==4){cp[3]|=(c<<27)|(t<<28);cp[4]|=f;}
    else {unsigned shift=5+9*(slot-5);cp[4]|=(c<<shift)|(t<<(shift+1))|(f<<(shift+4));}
   }
  }
  return true;
 }
};
}
