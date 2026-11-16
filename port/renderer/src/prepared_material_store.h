#pragma once
#include "prepared_material.h"
#include <cstdio>
#include <unordered_map>
#include <atomic>
namespace t2p {
class PreparedMaterialStore {
 struct Item{uint64_t key;uint32_t offset,size;};
 struct Cached{std::shared_ptr<const PreparedMaterial> program;uint64_t age;};
 struct Budget{std::atomic<size_t> bytes{0};};
 std::shared_ptr<Budget> budget=std::make_shared<Budget>();
 std::vector<Item> index;std::unordered_map<uint32_t,Cached> cache;FILE* file=nullptr;uint64_t clock=0;
 static uint32_t u32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
public:
 PreparedMaterialStore()=default;
 PreparedMaterialStore(const PreparedMaterialStore&)=delete;
 PreparedMaterialStore& operator=(const PreparedMaterialStore&)=delete;
 uint64_t hits=0,reads=0,misses=0;
 ~PreparedMaterialStore(){if(file)std::fclose(file);}
 size_t programs()const{return index.size();}
 bool open(const char* name){
  if(file)return !index.empty();FILE* f=std::fopen(name,"rb");if(!f)return false;
  auto fail=[&]{std::fclose(f);return false;};
  if(std::fseek(f,0,SEEK_END))return fail();const long length=std::ftell(f);if(length<12||length>64*1024*1024||std::fseek(f,0,SEEK_SET))return fail();
  uint8_t header[12];if(std::fread(header,1,12,f)!=12||std::memcmp(header,"WWMT",4)||u32(header+4)!=1)return fail();const auto n=u32(header+8);if(!n||n>16384||12+uint64_t(n)*16>uint64_t(length))return fail();
  std::vector<Item> fresh;fresh.reserve(n);for(unsigned i=0;i<n;++i){uint8_t row[16];if(std::fread(row,1,16,f)!=16)return fail();Item item{uint64_t(u32(row))|uint64_t(u32(row+4))<<32,u32(row+8),u32(row+12)};
   if(item.offset<12+n*16||item.size<40||item.size>32768||uint64_t(item.offset)+item.size>uint64_t(length)||(i&&item.key<fresh.back().key))return fail();fresh.push_back(item);
  }
  file=f;index=std::move(fresh);return true;
 }
 std::shared_ptr<const PreparedMaterial> resolve(const uint8_t* data,size_t size,const std::shared_ptr<const PreparedMaterial>& previous,std::vector<uint32_t>& args){
  if(previous&&previous->bind({data,size},args)){++hits;return previous;}
  if(!file){++misses;return {};}
  PreparedMaterial wanted;if(!PreparedMaterial::compile({data,size},wanted)){++misses;return {};}
  auto begin=std::lower_bound(index.begin(),index.end(),wanted.key,[](const Item& a,uint64_t b){return a.key<b;});
  for(auto at=begin;at!=index.end()&&at->key==wanted.key;++at){
   auto found=cache.find(at->offset);if(found!=cache.end()){if(found->second.program->bind({data,size},args)){found->second.age=++clock;++hits;return found->second.program;}continue;}
   while(cache.size()>=128){auto oldest=cache.begin();for(auto c=cache.begin();c!=cache.end();++c)if(c->second.age<oldest->second.age)oldest=c;cache.erase(oldest);}
   std::vector<uint8_t> bytes(at->size);if(std::fseek(file,at->offset,SEEK_SET)||std::fread(bytes.data(),1,bytes.size(),file)!=bytes.size())continue;++reads;
   auto p=std::make_unique<PreparedMaterial>();if(!PreparedMaterial::load(bytes,*p)||p->key!=at->key||!p->bind({data,size},args))continue;
   const auto used=p->bytes();if(used>(1u<<20)-std::min(size_t(1u<<20),budget->bytes.load(std::memory_order_relaxed)))continue;
   budget->bytes.fetch_add(used,std::memory_order_relaxed);auto owner=budget;
   std::shared_ptr<const PreparedMaterial> program(p.release(),[owner,used](const PreparedMaterial* value){delete value;owner->bytes.fetch_sub(used,std::memory_order_relaxed);});
   cache.emplace(at->offset,Cached{program,++clock});return program;
  }
  ++misses;return {};
 }
};
}
