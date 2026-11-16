#pragma once
#include "prepared_commands.h"
#include <array>
#include <memory>
#include <cstdio>
namespace t2p {
struct GxPrimitiveRunPart {const uint8_t* data;uint32_t count;uint8_t prim;};
// Only fields gxVertexSize / vertex decoding consume identify an executable
// layout. Unused VAT slots and ignored dequantization bits vary across SDKs.
inline std::array<uint32_t,5> preparedShapeFormat(std::array<uint32_t,5> cp){
 cp[0]&=0x1ffff;cp[1]&=0xffff;uint32_t a=0,b=0,c=0;
 if((cp[0]>>9)&3)a|=0x1ff;
 if((cp[0]>>11)&3)a|=(15u<<9)|(1u<<31);
 if((cp[0]>>13)&3)a|=15u<<13;
 if((cp[0]>>15)&3)a|=15u<<17;
 if(cp[1]&3)a|=0x1ffu<<21;
 for(unsigned t=1;t<=3;++t)if((cp[1]>>(2*t))&3)b|=0x1ffu<<(9*(t-1));
 if((cp[1]>>8)&3){b|=15u<<27;c|=31;}
 for(unsigned t=5;t<=7;++t)if((cp[1]>>(2*t))&3)c|=0x1ffu<<(5+9*(t-5));
 cp[2]&=a;cp[3]&=b;cp[4]&=c;return cp;
}
struct PreparedShape {
 PreparedShape()=default;PreparedShape(const PreparedShape&)=delete;PreparedShape& operator=(const PreparedShape&)=delete;PreparedShape(PreparedShape&&)=default;PreparedShape& operator=(PreparedShape&&)=default;
 struct Op {uint32_t raw=0,end=0,flat=0,count=0;uint8_t cmd=0;};
 std::array<uint32_t,5> format{};uint32_t vsize=0;uint8_t vat=0;
 std::vector<uint8_t> source,stream;
 std::vector<Op> ops;std::vector<GxPrimitiveRunPart> parts;
 std::vector<uint32_t> prefix,contiguous,ends512;
 size_t bytes()const{return sizeof(*this)+source.capacity()+stream.capacity()+ops.capacity()*sizeof(Op)+parts.capacity()*sizeof(GxPrimitiveRunPart)+(prefix.capacity()+contiguous.capacity()+ends512.capacity())*4;}
 static uint32_t be16(const uint8_t*p){return uint32_t(p[0])<<8|p[1];}
 bool compile(const uint8_t* data,size_t n,const std::array<uint32_t,5>& cp,uint32_t size){
  if(!data||!n||n>262144||!size||size>256)return false;
  PreparedShape fresh;fresh.format=preparedShapeFormat(cp);fresh.vsize=size;fresh.source.assign(data,data+n);bool first=true;
  for(size_t at=0;at<n;){if(!data[at]){++at;continue;}
   const uint8_t cmd=data[at],prim=cmd&0xf8;
   if(n-at<3 || (prim!=0x80&&prim!=0x88&&prim!=0x90&&prim!=0x98&&prim!=0xa0))return false;
   if(first){fresh.vat=cmd&7;first=false;}if((cmd&7)!=fresh.vat)return false;
   const uint32_t count=be16(data+at+1);if(count<(prim==0x80||prim==0x88?4u:3u))return false;
   const size_t bytes=size_t(count)*size;if(bytes>n-at-3)return false;
   Op op;op.raw=uint32_t(at);op.end=uint32_t(at+3+bytes);op.flat=uint32_t(fresh.stream.size());op.count=count;op.cmd=cmd;
   fresh.ops.push_back(op);fresh.stream.insert(fresh.stream.end(),data+at+3,data+at+3+bytes);at=op.end;
  }
  if(first)return false;fresh.finalize();*this=std::move(fresh);return true;
 }
 void finalize(){
  const size_t n=ops.size();parts.resize(n);prefix.resize(n+1);contiguous.resize(n);ends512.resize(n);prefix[0]=0;
  for(size_t i=0;i<n;++i){parts[i]={stream.data()+ops[i].flat,ops[i].count,uint8_t(ops[i].cmd&0xf8)};prefix[i+1]=prefix[i]+ops[i].count;}
  for(size_t i=n;i-->0;)contiguous[i]=(i+1<n&&ops[i].end==ops[i+1].raw)?contiguous[i+1]:uint32_t(i+1);
  for(size_t i=0;i<n;++i)ends512[i]=uint32_t(endFor(i,512));
 }
 size_t endFor(size_t i,uint32_t limit)const{
  if(i>=ops.size()||limit<6)return i;
  const size_t stop=std::min<size_t>(contiguous[i],i+128);
  const uint32_t goal=prefix[i]+limit;
  return size_t(std::upper_bound(prefix.begin()+i,prefix.begin()+stop+1,goal)-prefix.begin()-1);
 }
 bool serialize(std::vector<uint8_t>& out)const{
  if(source.empty()||ops.empty())return false;
  std::vector<uint8_t> b={'W','W','S','G'};auto word=[&](uint32_t v){for(int i=0;i<4;++i)b.push_back(uint8_t(v>>(i*8)));};
  word(1);for(auto v:format)word(v);word(vsize);word(vat);word(uint32_t(source.size()));word(uint32_t(stream.size()));word(uint32_t(ops.size()));
  b.insert(b.end(),source.begin(),source.end());b.insert(b.end(),stream.begin(),stream.end());
  for(auto op:ops){word(op.raw);word(op.end);word(op.flat);word(op.count);word(op.cmd);}
  uint64_t h=PreparedCommands::checksum(b.data(),b.size());word(uint32_t(h));word(uint32_t(h>>32));out.swap(b);return true;
 }
 static bool load(const uint8_t* b,size_t n,const uint8_t* expected,size_t length,const std::array<uint32_t,5>& cp,PreparedShape& out){
  if(!b||!expected||n<56||n>2*1024*1024||std::memcmp(b,"WWSG",4))return false;
  auto word=[&](size_t at){return uint32_t(b[at])|uint32_t(b[at+1])<<8|uint32_t(b[at+2])<<16|uint32_t(b[at+3])<<24;};
  const uint32_t raw=word(36),flat=word(40),count=word(44);if(word(4)!=1||raw!=length||raw>262144||flat>262144||!count||count>65536||48+size_t(raw)+flat+20*size_t(count)+8!=n)return false;
  if(PreparedCommands::checksum(b,n-8)!=(uint64_t(word(n-8))|uint64_t(word(n-4))<<32))return false;
  for(unsigned i=0;i<5;++i)if(word(8+i*4)!=preparedShapeFormat(cp)[i])return false;
  if(std::memcmp(b+48,expected,raw))return false;
  // Semantic verification only once at binding: compile a reference from this
  // immutable source and compare every prepared operation and flattened byte.
  // The hot execution path never walks its GX headers again.
  PreparedShape p;if(!p.compile(expected,length,cp,word(28))||p.vat!=word(32)||p.ops.size()!=count||p.stream.size()!=flat||std::memcmp(p.stream.data(),b+48+raw,flat))return false;
  size_t at=48+raw+flat;for(auto op:p.ops){if(word(at)!=op.raw||word(at+4)!=op.end||word(at+8)!=op.flat||word(at+12)!=op.count||word(at+16)!=op.cmd)return false;at+=20;}
  // The original stays in the immutable game resource; retain only executable
  // operations and contiguous indices. No second decoded-geometry cache.
  std::vector<uint8_t>().swap(p.source);out=std::move(p);return true;
 }
};
}
namespace t2p {
inline uint64_t preparedShapeKey(const uint8_t* source,size_t n,const std::array<uint32_t,5>& cp){uint64_t h=PreparedCommands::checksum(source,n);for(auto v:preparedShapeFormat(cp))for(int i=0;i<4;++i)h=(h^uint8_t(v>>(8*i)))*1099511628211ull;return h;}
class PreparedShapeStore {
 struct Slot {const uint8_t* pointer=nullptr;uint32_t size=0,generation=0;std::array<uint32_t,5> format{};uint64_t age=0;std::unique_ptr<PreparedShape> program;};
 std::array<Slot,512> slots_{};FILE* file_=nullptr;std::vector<uint8_t> index_;uint32_t entries_=0;long length_=0;size_t bytes_=0;uint64_t clock_=0;
 static uint32_t word(const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
 bool readAt(long at,void* data,size_t size){return at>=0&&at<=length_&&size<=size_t(length_-at)&&!std::fseek(file_,at,SEEK_SET)&&std::fread(data,1,size,file_)==size;}
 void evict(Slot& s){if(s.program){bytes_-=s.program->bytes();s.program.reset();}s.pointer=nullptr;}
public:
 uint64_t hits=0,misses=0,refused=0;size_t bytes()const{return bytes_+index_.capacity();}
 ~PreparedShapeStore(){if(file_)std::fclose(file_);}
 bool open(const char* path){if(file_){std::fclose(file_);file_=nullptr;}for(auto& s:slots_)evict(s);entries_=0;
  FILE* f=std::fopen(path,"rb");if(!f)return false;
  if(std::fseek(f,0,SEEK_END)||(length_=std::ftell(f))<12){std::fclose(f);return false;}file_=f;
  uint8_t header[12];if(!readAt(0,header,12)||std::memcmp(header,"WWSP",4)||word(header+4)!=1||(entries_=word(header+8))>65536||12+size_t(entries_)*16>size_t(length_)){std::fclose(file_);file_=nullptr;return false;}
  try{index_.resize(size_t(entries_)*16);if(!readAt(12,index_.data(),index_.size())){std::fclose(file_);file_=nullptr;return false;}}
  catch(...){std::fclose(file_);file_=nullptr;return false;}
  return true;
 }
 const PreparedShape* get(const uint8_t* p,uint32_t size,uint32_t generation,const std::array<uint32_t,5>& rawFormat){
  const auto format=preparedShapeFormat(rawFormat);
  if(!file_||!p||!size)return nullptr;
  const uintptr_t ptr=reinterpret_cast<uintptr_t>(p);const size_t base=(((ptr>>5)^(ptr>>15))&127)*4;
  Slot* slot=&slots_[base];
  for(size_t i=base;i<base+4;++i){auto& s=slots_[i];if(s.pointer==p&&s.size==size&&s.generation==generation&&s.format==format){s.age=++clock_;if(s.program)++hits;return s.program.get();}if(!s.pointer||s.age<slot->age)slot=&s;}
  ++misses;evict(*slot);slot->pointer=p;slot->size=size;slot->generation=generation;slot->format=format;slot->age=++clock_;
  try{
   const uint64_t key=preparedShapeKey(p,size,format);uint32_t lo=0,hi=entries_;uint8_t record[16];
   while(lo<hi){uint32_t middle=lo+(hi-lo)/2;std::memcpy(record,index_.data()+size_t(middle)*16,16);uint64_t found=uint64_t(word(record))|uint64_t(word(record+4))<<32;if(found<key)lo=middle+1;else hi=middle;}
   for(uint32_t i=lo;i<entries_;++i){std::memcpy(record,index_.data()+size_t(i)*16,16);uint64_t found=uint64_t(word(record))|uint64_t(word(record+4))<<32;if(found!=key)break;
    const uint32_t offset=word(record+8),n=word(record+12);if(n>2*1024*1024||offset>uint32_t(INT32_MAX)){++refused;continue;}
    std::vector<uint8_t> data(n);if(!readAt(long(offset),data.data(),data.size())){++refused;continue;}
    auto fresh=std::make_unique<PreparedShape>();if(!PreparedShape::load(data.data(),data.size(),p,size,format,*fresh)){++refused;continue;}
    const size_t cost=fresh->bytes();if(cost>1024*1024){++refused;return nullptr;}
    while(bytes_>1024*1024-cost){Slot* oldest=nullptr;for(auto& s:slots_)if(&s!=slot&&s.program&&(!oldest||s.age<oldest->age))oldest=&s;if(!oldest){++refused;return nullptr;}evict(*oldest);}
    bytes_+=cost;slot->program=std::move(fresh);return slot->program.get();
   }
  }catch(...){++refused;}
  return nullptr;
 }
};
}
