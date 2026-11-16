#pragma once
#include "uniform_recipe.h"
#include <array>
#include <span>
namespace t2p {
// A group consumes member-owned packed inputs, never a previous frame's poses.
// Duplicate destinations validate the aliasing that made matrix batching legal.
struct DirectGroupRows {
 struct Copy {uint16_t destination,member,source;};
 std::vector<Copy> copies;
 std::vector<uint16_t> zero;
 uint16_t rows=0,members=0;
 static int sourceOf(const UniformRecipe& recipe,const UniformRecipe::Op& wanted,unsigned index){
  for(const auto& o:recipe.ops)if(o.source==wanted.source&&o.index==index&&o.row==wanted.row&&o.components==wanted.components)return o.destination;return -1;
 }
 bool compile(const UniformRecipe& group,std::span<const UniformRecipe* const> own,std::span<const std::array<int8_t,10>> slots,unsigned palette){
  if(own.size()<2||own.size()>64||slots.size()!=own.size()||palette>10||group.destinationRows>96)return false;
  DirectGroupRows fresh;fresh.rows=group.destinationRows;fresh.members=uint16_t(own.size());
  for(const auto& op:group.ops){bool assigned=false;
   if(op.source==UniformRecipe::Position||op.source==UniformRecipe::Normal){
    if(op.index>=palette){
     int source=own[0]?sourceOf(*own[0],op,op.index):-1;
     if(source>=0)fresh.copies.push_back({op.destination,0,uint16_t(source)});else fresh.zero.push_back(op.destination);
     continue;
    }
    for(unsigned m=0;m<own.size();++m){if(!own[m])return false;for(unsigned k=0;k<10;++k)if(slots[m][k]==int(op.index)){
      const int source=sourceOf(*own[m],op,k);if(source<0)return false;
      fresh.copies.push_back({op.destination,uint16_t(m),uint16_t(source)});assigned=true;
    }}
   }else{
    for(unsigned m=0;m<own.size();++m){if(!own[m])return false;const int source=sourceOf(*own[m],op,op.index);if(source<0)return false;fresh.copies.push_back({op.destination,uint16_t(m),uint16_t(source)});assigned=true;}
   }
   if(!assigned)return false;
  }
  *this=std::move(fresh);return true;
 }
 bool apply(std::span<const std::span<const PVec>> input,std::span<PVec> output)const{
  if(input.size()!=members||output.size()<rows||rows>96)return false;
  std::array<PVec,96> temporary{};std::array<bool,96> filled{};
  for(auto at:zero){if(at>=rows)return false;filled[at]=true;}
  for(const auto& copy:copies){if(copy.member>=input.size()||copy.destination>=rows||copy.source>=input[copy.member].size())return false;
   const auto& value=input[copy.member][copy.source];
   if(filled[copy.destination]){if(std::memcmp(&temporary[copy.destination],&value,sizeof(PVec)))return false;}
   else{temporary[copy.destination]=value;filled[copy.destination]=true;}
  }
  std::memcpy(output.data(),temporary.data(),size_t(rows)*sizeof(PVec));return true;
 }
};
}
