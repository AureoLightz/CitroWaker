#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>
namespace t2p {
// PC-decoded MDL3 instruction layout. Values patched by animation/texture binding
// remain arguments; command addresses, counts and ordering remain immutable.
struct PreparedMaterial {
 struct Op {uint32_t kind,address,count,arg;bool operator==(const Op&)const=default;};
 struct Guard {uint32_t offset,size;bool operator==(const Guard&)const=default;};
 std::vector<uint8_t> layout;
 std::vector<Op> ops;std::vector<Guard> guards;std::vector<uint32_t> source;
 uint64_t key=0;
 static uint32_t be(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
 static uint64_t hash(std::span<const uint8_t> p){uint64_t h=14695981039346656037ull;for(auto c:p)h=(h^c)*1099511628211ull;return h;}
 static bool compile(std::span<const uint8_t> input,PreparedMaterial& out){
  if(input.empty()||input.size()>2048)return false;
  PreparedMaterial p;p.layout.assign(input.begin(),input.end());
  auto guard=[&](size_t at,size_t n){p.guards.push_back({uint32_t(at),uint32_t(n)});};
  auto value=[&](size_t at,unsigned first){p.source.push_back(uint32_t(at));for(unsigned k=first;k<4;++k)p.layout[at+k]=0;};
  for(size_t at=0;at<input.size();){auto c=input[at];
   if(!c){const auto start=at;while(at<input.size()&&!input[at])++at;guard(start,at-start);continue;}
   Op op{c,0,1,uint32_t(p.source.size())};
   if(c==0x61){if(input.size()-at<5)return false;op.address=input[at+1];if(op.address==0x45||op.address==0x52||op.address==0x65)return false;guard(at,2);value(at+1,1);at+=5;}
   else if(c==8){if(input.size()-at<6)return false;op.address=input[at+1];guard(at,2);value(at+2,0);at+=6;}
   else if(c==0x10){if(input.size()-at<5)return false;auto h=be(input.data()+at+1);op.address=h&65535;op.count=(h>>16)+1;if(op.address>=0x1100||op.count>0x1100-op.address||op.count>(input.size()-at-5)/4)return false;guard(at,5);for(unsigned k=0;k<op.count;++k)value(at+5+4*k,0);at+=5+4*op.count;}
   else return false;
   p.ops.push_back(op);
  }
  if(p.ops.empty())return false;p.key=hash(p.layout);out=std::move(p);return true;
 }
 bool matches(std::span<const uint8_t> bytes)const{
  if(bytes.size()!=layout.size())return false;
  for(const auto& g:guards)if(g.offset>bytes.size()||g.size>bytes.size()-g.offset||std::memcmp(bytes.data()+g.offset,layout.data()+g.offset,g.size))return false;
  return true;
 }
 bool bind(std::span<const uint8_t> bytes,std::vector<uint32_t>& arguments)const{
  if(!matches(bytes))return false;std::vector<uint32_t> fresh;fresh.reserve(source.size());for(auto at:source){if(at>bytes.size()||bytes.size()-at<4)return false;fresh.push_back(be(bytes.data()+at));}arguments.swap(fresh);return true;
 }
 template<class F>bool apply(F& fifo,std::span<const uint32_t> args)const{
  if(args.size()!=source.size())return false;
  // Reject bad input before any canonical-register effect.
  for(const auto& op:ops)if(op.arg>args.size()||op.count>args.size()-op.arg||(op.kind==0x61&&(args[op.arg]>>24)!=op.address))return false;
  for(const auto& op:ops){if(op.kind==0x61)fifo.applyBp(args[op.arg]);else if(op.kind==8)fifo.applyCp(uint8_t(op.address),args[op.arg]);else fifo.applyProgramXf(op.address,args.data()+op.arg,op.count);}return true;
 }
 bool serialize(std::vector<uint8_t>& output)const{
  if(layout.empty()||layout.size()>2048||ops.empty())return false;std::vector<uint8_t> b={'C','W','M','T'};
  auto word=[&](uint32_t v){for(unsigned i=0;i<4;++i)b.push_back(uint8_t(v>>(8*i)));};auto wide=[&](uint64_t v){word(uint32_t(v));word(uint32_t(v>>32));};
  word(1);word(uint32_t(layout.size()));word(uint32_t(ops.size()));word(uint32_t(guards.size()));word(uint32_t(source.size()));wide(key);b.insert(b.end(),layout.begin(),layout.end());
  for(auto o:ops){word(o.kind);word(o.address);word(o.count);word(o.arg);}for(auto g:guards){word(g.offset);word(g.size);}for(auto v:source)word(v);wide(hash(b));output.swap(b);return true;
 }
 static bool load(std::span<const uint8_t> b,PreparedMaterial& output){
  if(b.size()<40||b.size()>32768||std::memcmp(b.data(),"CWMT",4))return false;
  size_t at=4;bool ok=true;auto word=[&](){if(at>b.size()||b.size()-at<4){ok=false;return uint32_t(0);}auto p=b.data()+at;at+=4;return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;};auto wide=[&](){auto lo=word();return uint64_t(lo)|uint64_t(word())<<32;};
  if(word()!=1)return false;auto n=word(),no=word(),ng=word(),ns=word();auto key=wide();if(!ok||n>2048||no>512||ng>1024||ns>512||uint64_t(at)+n+uint64_t(no)*16+uint64_t(ng)*8+uint64_t(ns)*4+8!=b.size())return false;
  PreparedMaterial p;auto layout=b.subspan(at,n);at+=n;
  // Recompile only as a cold-load validator. Execution consumes the checked PC
  // metadata below, and never reparses headers for subsequent calls.
  PreparedMaterial check;if(!compile(layout,check)||check.key!=key||check.ops.size()!=no||check.guards.size()!=ng||check.source.size()!=ns)return false;
  p.layout.assign(layout.begin(),layout.end());p.key=key;
  for(unsigned i=0;i<no;++i){Op o{word(),word(),word(),word()};if(o!=check.ops[i])return false;p.ops.push_back(o);}
  for(unsigned i=0;i<ng;++i){Guard g{word(),word()};if(g!=check.guards[i])return false;p.guards.push_back(g);}
  for(unsigned i=0;i<ns;++i){auto value=word();if(value!=check.source[i])return false;p.source.push_back(value);}
  const auto checksum=wide();if(!ok||at!=b.size()||hash(b.first(b.size()-8))!=checksum)return false;output=std::move(p);return true;
 }
 size_t bytes()const{return sizeof(*this)+layout.capacity()+ops.capacity()*sizeof(Op)+guards.capacity()*sizeof(Guard)+source.capacity()*4;}
};
}
