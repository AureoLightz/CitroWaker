#pragma once
#include "gxstateprogram.h"
#include "material_versions.h"
#include <array>
#include <atomic>
#include <memory>
namespace t2p {
void prepareStateProgram(GxStateProgram&);
// Single producer; queued tokens own a lease until the FIFO consumer releases it.
// No game pointer escapes registration and only idle slots can be replaced.
class MaterialProgramPool {
 struct Slot { std::atomic<uint32_t> refs{0}; uint64_t token=0; uint32_t hash=0; std::unique_ptr<GxStateProgram> program; };
 struct Memo { const uint8_t* source=nullptr; uint64_t token=0; };
 static size_t memoIndex(const uint8_t* p){uint32_t x=uint32_t(reinterpret_cast<uintptr_t>(p)>>5);x^=x>>11;x*=0x9e3779b1u;return (x^(x>>16))&511;}
 std::array<Slot,512> slots_{};
 std::array<Memo,512> memo_{};
 struct VersionMemo {MaterialVersion version;uint64_t token=0;};
 std::array<VersionMemo,512> versions_{};
 uint64_t generation_=0; size_t bytes_=0, next_=0,budget_=256*1024;
public:
 std::atomic<uint32_t> versionHits{0},versionMisses{0},versionAuditFailures{0},preparedCalls{0};
 void setBudget(size_t n){if(n>=bytes_&&n<=2u*1024*1024)budget_=n;}
 template<class Resolver>uint64_t publishPrepared(const uint8_t* data,size_t size,const MaterialVersion& version,bool audit,Resolver&& resolve){
  if(!version.valid()||version.source!=reinterpret_cast<uintptr_t>(data)||version.size!=size)return publish(data,size);
  auto& memo=versions_[memoIndex(data)];
  std::shared_ptr<const PreparedMaterial> previous;
  if(memo.token){auto& slot=slots_[memo.token&511];if(slot.token==memo.token&&slot.program){
   previous=slot.program->prepared;
   if(memo.version==version&&slot.program->bytes.size()==size){
    if(audit&&std::memcmp(slot.program->bytes.data(),data,size))versionAuditFailures.fetch_add(1,std::memory_order_relaxed);
    else{slot.refs.fetch_add(1,std::memory_order_relaxed);versionHits.fetch_add(1,std::memory_order_relaxed);if(previous)preparedCalls.fetch_add(1,std::memory_order_relaxed);return memo.token;}
   }
  }}
  std::vector<uint32_t> args;std::shared_ptr<const PreparedMaterial> schema;
  // Optional PC metadata must never turn memory pressure into a game failure.
  try{schema=resolve(data,size,previous,args);}catch(const std::bad_alloc&){return 0;}
  if(!schema)return publishVersioned(data,size,version,audit);
  size_t at=slots_.size();for(size_t i=0;i<slots_.size();++i){auto k=(next_+i)%slots_.size();if(!slots_[k].refs.load(std::memory_order_acquire)){at=k;break;}}if(at==slots_.size())return 0;
  try{auto p=std::make_unique<GxStateProgram>();p->prepared=std::move(schema);p->words=std::move(args);p->bytes.assign(data,data+size);auto& slot=slots_[at];const auto old=slot.program?slot.program->capacityBytes():0;
   if(p->capacityBytes()>budget_-(bytes_-old))return 0;bytes_=bytes_-old+p->capacityBytes();slot.program=std::move(p);slot.hash=0;slot.token=(++generation_<<9)|at;slot.refs.store(1,std::memory_order_release);next_=(at+1)%slots_.size();memo={version,slot.token};memo_[memoIndex(data)]={data,slot.token};
   versionMisses.fetch_add(1,std::memory_order_relaxed);preparedCalls.fetch_add(1,std::memory_order_relaxed);return slot.token;
  }catch(...){return 0;}
 }

 uint64_t publishVersioned(const uint8_t* data,size_t size,const MaterialVersion& version,bool audit=false){
  if(!version.valid()||version.source!=reinterpret_cast<uintptr_t>(data)||version.size!=size)return publish(data,size);
  auto& memo=versions_[memoIndex(data)];
  if(memo.token&&memo.version==version){auto& slot=slots_[memo.token&511];
   if(slot.token==memo.token&&slot.program&&slot.program->bytes.size()==size){
    if(audit&&std::memcmp(slot.program->bytes.data(),data,size))versionAuditFailures.fetch_add(1,std::memory_order_relaxed);
    else{slot.refs.fetch_add(1,std::memory_order_relaxed);versionHits.fetch_add(1,std::memory_order_relaxed);return memo.token;}
   }
  }
  versionMisses.fetch_add(1,std::memory_order_relaxed);const auto token=publish(data,size);if(token)memo={version,token};return token;
 }
 uint64_t publish(const uint8_t* data,size_t size) {
  if(!data || !size || size>2048)return 0;
  auto& memo=memo_[memoIndex(data)];
  if(memo.source==data && memo.token){auto& s=slots_[memo.token&511];
   if(s.token==memo.token && s.program && s.program->bytes.size()==size && !std::memcmp(s.program->bytes.data(),data,size)){
    s.refs.fetch_add(1,std::memory_order_relaxed);return s.token;
   }
  }
  if(!GxStateProgram::valid(data,size))return 0;
  uint32_t hash=2166136261u;size_t at=0;
  for(;at+4<=size;at+=4){uint32_t w;std::memcpy(&w,data+at,4);hash=(hash^w)*0x9e3779b1u;}
  for(;at<size;++at)hash=(hash^data[at])*16777619u;
  // Refuse commands which can notify a sink, rather than moving their execution.
  for(size_t i=0;i<size;){auto c=data[i];if(!c){++i;continue;}
   if(c==0x61){if(data[i+1]==0x52 || data[i+1]==0x65)return 0;i+=5;}
   else if(c==8)i+=6;else i+=5+4*((GxStateProgram::word(data+i+1)>>16)+1);
  }
  for(auto& s:slots_)if(s.program && s.hash==hash && s.program->bytes.size()==size && !std::memcmp(s.program->bytes.data(),data,size)){
   // Producer owns all registrations; the consumer only decrements refs.
   s.refs.fetch_add(1,std::memory_order_relaxed);memo={data,s.token};return s.token;
  }
  size_t slot=slots_.size();
  for(size_t i=0;i<slots_.size();++i){auto k=(next_+i)%slots_.size();if(!slots_[k].refs.load(std::memory_order_acquire)){slot=k;break;}}
  if(slot==slots_.size())return 0;
  try {
   auto p=std::make_unique<GxStateProgram>();if(!p->compile(data,size))return 0;prepareStateProgram(*p);
   auto& s=slots_[slot];auto old=s.program?s.program->capacityBytes():0;
   if(p->capacityBytes()>budget_-(bytes_-old))return 0;
   bytes_=bytes_-old+p->capacityBytes();s.program=std::move(p);
   s.hash=hash;s.token=(++generation_<<9)|slot;s.refs.store(1,std::memory_order_release);next_=(slot+1)%slots_.size();memo={data,s.token};return s.token;
  } catch(...) { return 0; }
 }
 // Only call for a token obtained from the ordered queue. Its outstanding lease
 // prevents replacement, including while apply() reads vectors on another core.
 const GxStateProgram* borrowed(uint64_t token)const {
  const auto& s=slots_[token&511];
  return token && s.refs.load(std::memory_order_acquire) && s.token==token?s.program.get():nullptr;
 }
 void release(uint64_t token){auto& s=slots_[token&511];s.refs.fetch_sub(1,std::memory_order_release);}
 size_t bytes()const{return bytes_;}
};
}
