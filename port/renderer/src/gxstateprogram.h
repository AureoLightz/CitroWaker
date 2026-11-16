#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "prepared_material.h"
namespace t2p {
struct GxStateProgram {
 struct Op { uint32_t value, offset; uint16_t count, address; uint8_t kind; };
 std::vector<uint8_t> bytes;
 std::vector<Op> ops;
 std::vector<uint32_t> words;
 std::shared_ptr<const PreparedMaterial> prepared;
 size_t capacityBytes()const{return bytes.capacity()+ops.capacity()*sizeof(Op)+words.capacity()*sizeof(uint32_t)+sizeof(*this);}
 static uint32_t word(const uint8_t* p) { return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3]; }
 // Validate first: refusing a program must have NO register/draw side effects.
 static bool valid(const uint8_t* p,size_t n) {
  if(!p||!n||n>2048)return false;
  for(size_t i=0;i<n;){const uint8_t c=p[i];if(c==0){++i;continue;}
   if(c==0x61){if(n-i<5 || p[i+1]==0x45)return false;i+=5;}
   else if(c==8){if(n-i<6)return false;i+=6;}
   else if(c==0x10){if(n-i<5)return false;const size_t count=(word(p+i+1)>>16)+1;if(count>(n-i-5)/4)return false;i+=5+4*count;}
   else return false;
  } return true;
 }
 bool compile(const uint8_t* p,size_t n) {
  if(!valid(p,n))return false;
  bytes.assign(p,p+n);ops.clear();words.clear();prepared.reset();
  for(size_t i=0;i<n;){const uint8_t c=p[i];if(!c){++i;continue;}Op o{};o.kind=c;
   if(c==0x61){o.value=word(p+i+1);i+=5;}
   else if(c==8){o.address=p[i+1];o.value=word(p+i+2);i+=6;}
   else {auto v=word(p+i+1);o.address=uint16_t(v);o.count=uint16_t((v>>16)+1);o.offset=uint32_t(words.size());for(unsigned k=0;k<o.count;++k)words.push_back(word(p+i+5+4*k));i+=5+4*o.count;}
   ops.push_back(o);
  }return true;
 }
 template<class F> bool apply(F& fifo)const {
  if(prepared)return prepared->apply(fifo,words);
  for(const auto& o:ops){if(o.kind==0x61)fifo.applyProgramBp(o.value,o.offset);else if(o.kind==8)fifo.applyCp(uint8_t(o.address),o.value);else fifo.applyProgramXf(o.address,words.data()+o.offset,o.count);}
  return true;
 }
};
} // namespace t2p
