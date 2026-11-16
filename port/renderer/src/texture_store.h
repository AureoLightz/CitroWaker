#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace t2p {
// PC splits the unchanged WWTP byte stream into small files. This bounds FAT
// traversal on random seeks; whether that beats conversion needs a console A/B.
// One 8 KiB page, two FILE handles, single render owner, never background I/O.
class TextureStore {
 public:
  struct Stats { uint64_t opens=0,reads=0,bytes=0,hits=0; };
  ~TextureStore(){close();}
  TextureStore()=default;
  TextureStore(const TextureStore&)=delete;
  TextureStore& operator=(const TextureStore&)=delete;
  void close(){for(auto& h:handles_){if(h.f)std::fclose(h.f);h=Handle{};}root_.clear();size_=0;chunk_=0;pageAt_=UINT64_MAX;age_=0;std::vector<uint8_t>().swap(page_);stats_={};}
  bool open(const char* root){
    close();if(!root)return false;
    const std::string path=std::string(root)+"/store.bin";
    FILE* f=std::fopen(path.c_str(),"rb");if(!f)return false;
    uint8_t b[24];const bool ok=std::fread(b,1,sizeof b,f)==sizeof b && std::fgetc(f)==EOF;std::fclose(f);
    if(!ok||std::memcmp(b,"WWSC",4)||u32(b+4)!=1)return false;
    const uint32_t chunk=u32(b+8),count=u32(b+20);const uint64_t size=uint64_t(u32(b+12))|(uint64_t(u32(b+16))<<32);
    if(chunk<(1u<<18)||chunk>(1u<<21)||(chunk&(chunk-1))||!size||size>uint64_t(chunk)*4096||count!=(size+chunk-1)/chunk)return false;
    try{page_.resize(kPage);}catch(...){return false;}
    root_=root;chunk_=chunk;size_=size;return true;
  }
  bool read(uint64_t offset,size_t n,void* dst){
    if(!chunk_||offset>size_||uint64_t(n)>size_-offset||(!dst&&n))return false;
    auto* out=static_cast<uint8_t*>(dst);
    while(n){
      const uint64_t at=offset&~uint64_t(kPage-1);
      const size_t within=size_t(offset-at),available=size_t(std::min<uint64_t>(kPage,size_-at));
      if(pageAt_==at){const size_t take=std::min(n,available-within);std::memcpy(out,page_.data()+within,take);stats_.hits++;out+=take;offset+=take;n-=take;continue;}
      // Large textures go straight into the caller's GPU allocation. No extra
      // buffer or double copy; split only at a bounded physical-file boundary.
      if(n>=kPage){const size_t take=std::min<uint64_t>(n,chunk_-(offset%chunk_));if(!physical(offset,take,out))return false;out+=take;offset+=take;n-=take;continue;}
      pageAt_=UINT64_MAX;
      if(!physical(at,available,page_.data()))return false;
      pageAt_=at;
      const size_t take=std::min(n,available-within);std::memcpy(out,page_.data()+within,take);out+=take;offset+=take;n-=take;
    }
    return true;
  }
  const Stats& stats()const{return stats_;}
 private:
  static constexpr size_t kPage=8u<<10;
  struct Handle{FILE* f=nullptr;uint32_t id=UINT32_MAX;uint64_t age=0;};
  Handle handles_[2];std::string root_;uint64_t size_=0,pageAt_=UINT64_MAX,age_=0;uint32_t chunk_=0;std::vector<uint8_t> page_;Stats stats_;
  static uint32_t u32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  bool physical(uint64_t offset,size_t n,void* dst){
    const uint32_t id=uint32_t(offset/chunk_);Handle* h=nullptr;
    for(auto& x:handles_)if(x.f&&x.id==id){h=&x;break;}
    if(!h){h=(!handles_[0].f||handles_[0].age<=handles_[1].age)?&handles_[0]:&handles_[1];if(h->f)std::fclose(h->f);*h=Handle{};
      char name[24];std::snprintf(name,sizeof name,"/%04lx.bin",(unsigned long)id);h->f=std::fopen((root_+name).c_str(),"rb");if(!h->f)return false;
      std::setvbuf(h->f,nullptr,_IONBF,0);h->id=id;stats_.opens++;
    }
    h->age=++age_;stats_.reads++;stats_.bytes+=n;
    if(std::fseek(h->f,long(offset%chunk_),SEEK_SET)||std::fread(dst,1,n,h->f)!=n){std::fclose(h->f);*h=Handle{};pageAt_=UINT64_MAX;return false;}
    return true;
  }
};
}
