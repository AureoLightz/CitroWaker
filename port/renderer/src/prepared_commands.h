#pragma once
// Portable prepared PICA command sequences. The disk representation never holds
// a host pointer or a C++ object image. Binding is transactional before emission.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
namespace t2p {
struct PreparedCommands {
 static constexpr uint32_t version=1, maxWords=16384, maxPatches=4096;
 struct Patch {uint32_t word=0;uint16_t input=0;uint32_t mask=~0u;};
 // Eight ordered state groups, matching the executor's shadow-state boundaries.
 std::array<uint32_t,9> offsets{};
 std::vector<uint32_t> words;
 std::vector<Patch> patches;
 std::vector<uint8_t> key; // canonical consumed inputs, exact bytes, not hash alone
 uint64_t fingerprint=0;
 bool valid(std::string* why=nullptr)const {
  auto fail=[&](const char* text){if(why)*why=text;return false;};
  if(!fingerprint || key.empty() || key.size()>4096 || words.empty() || words.size()>maxWords || patches.size()>maxPatches)return fail("prepared command limits");
  if(offsets[0] || offsets[8]!=words.size())return fail("prepared group bounds");
  std::vector<uint8_t> data(words.size(),0);
  for(unsigned group=0;group<8;++group){size_t at=offsets[group],end=offsets[group+1];
   if(at>end || end>words.size() || (at&1) || (end&1))return fail("prepared group alignment");
   while(at<end){if(end-at<2)return fail("prepared packet header");
    const uint32_t header=words[at+1];const size_t extra=(header>>20)&0x7ff;
    const size_t size=(2+extra+1)&~size_t(1);
    if(size>end-at)return fail("prepared packet extent");
    const unsigned reg=header&0xffff;
    if(reg>=0x300 || ((header>>31) && extra>=0x300-reg))return fail("prepared register range");
    data[at]=1;for(size_t n=0;n<extra;++n)data[at+2+n]=1;
    if((2+extra)&1)if(words[at+size-1])return fail("prepared nonzero packet padding");
    at+=size;
   }
  }
  // Overlapping writes to the same word make relocation order-dependent.
  std::vector<uint32_t> masks(words.size(),0);
  for(const auto& p:patches){if(p.word>=words.size() || !data[p.word] || p.input>=1024 || !p.mask || (masks[p.word]&p.mask))return fail("prepared patch target");masks[p.word]|=p.mask;}
  return true;
 }
 bool bindInto(const uint32_t* inputs,size_t count,uint32_t* output,size_t capacity)const {
  // No allocations and no output modification on refusal. Inputs cannot alias
  // the destination: relocation must not read values overwritten by the copy.
  if(!output || capacity<words.size() || (!inputs && !patches.empty()))return false;
  const uintptr_t a=reinterpret_cast<uintptr_t>(inputs),b=reinterpret_cast<uintptr_t>(output);
  if(count>SIZE_MAX/4 || words.size()>SIZE_MAX/4)return false;
  if(count && a<=UINTPTR_MAX-count*4 && b<=UINTPTR_MAX-words.size()*4 && a<b+words.size()*4 && b<a+count*4)return false;
  for(const auto& p:patches)if(p.input>=count || p.word>=words.size() || (inputs[p.input]&~p.mask))return false;
  std::memcpy(output,words.data(),words.size()*4);
  for(const auto& p:patches)output[p.word]=(output[p.word]&~p.mask)|(inputs[p.input]&p.mask);
  return true;
 }
 bool bind(const uint32_t* inputs,size_t count,std::vector<uint32_t>& out)const {
  if(!inputs && !patches.empty())return false;
  for(const auto& p:patches)if(p.input>=count || p.word>=words.size() || (inputs[p.input]&~p.mask))return false;
  std::vector<uint32_t> fresh(words.size());if(!bindInto(inputs,count,fresh.data(),fresh.size()))return false;
  out.swap(fresh);return true;
 }
 static uint64_t checksum(const uint8_t* p,size_t n){uint64_t h=14695981039346656037ull;for(size_t i=0;i<n;++i)h=(h^p[i])*1099511628211ull;return h;}
 bool serialize(std::vector<uint8_t>& out)const {
  if(!valid())return false;std::vector<uint8_t> fresh={'C','W','C','P'};
  auto u32=[&](uint32_t v){for(int i=0;i<4;++i)fresh.push_back(uint8_t(v>>(8*i)));};
  auto u64=[&](uint64_t v){u32(uint32_t(v));u32(uint32_t(v>>32));};
  u32(version);u64(fingerprint);u32(uint32_t(key.size()));u32(uint32_t(words.size()));u32(uint32_t(patches.size()));
  for(auto v:offsets)u32(v);fresh.insert(fresh.end(),key.begin(),key.end());
  for(auto v:words)u32(v);for(auto p:patches){u32(p.word);u32(p.input);u32(p.mask);}
  const uint64_t sum=checksum(fresh.data(),fresh.size());u64(sum);out.swap(fresh);return true;
 }
 static bool load(const uint8_t* bytes,size_t size,uint64_t expected,PreparedCommands& out,std::string* why=nullptr){
  auto fail=[&](const char* text){if(why)*why=text;return false;};
  if(!bytes || size<72 || size>128*1024 || std::memcmp(bytes,"CWCP",4))return fail("prepared command header");
  auto u32=[&](size_t at){return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;};
  auto u64=[&](size_t at){return uint64_t(u32(at))|uint64_t(u32(at+4))<<32;};
  if(u32(4)!=version || !expected || u64(8)!=expected)return fail("prepared command build mismatch");
  const uint32_t keys=u32(16),words=u32(20),patches=u32(24);
  if(!keys || keys>4096 || !words || words>maxWords || patches>maxPatches)return fail("prepared command file limits");
  const size_t required=64+size_t(keys)+4*size_t(words)+12*size_t(patches)+8;
  if(required!=size || checksum(bytes,size-8)!=u64(size-8))return fail("prepared command checksum/length");
  PreparedCommands fresh;fresh.fingerprint=expected;for(unsigned i=0;i<9;++i)fresh.offsets[i]=u32(28+4*i);
  size_t at=64;fresh.key.assign(bytes+at,bytes+at+keys);at+=keys;
  fresh.words.resize(words);for(auto& v:fresh.words){v=u32(at);at+=4;}
  fresh.patches.resize(patches);for(auto& p:fresh.patches){p.word=u32(at);const uint32_t input=u32(at+4);if(input>=1024)return fail("prepared patch input");p.input=uint16_t(input);p.mask=u32(at+8);at+=12;}
  if(!fresh.valid(why))return false;out=std::move(fresh);return true;
 }
};
} // namespace t2p
