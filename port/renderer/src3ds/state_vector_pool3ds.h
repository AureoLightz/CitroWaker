#pragma once
#include "gxstate.h"
#include <cstddef>
#include <utility>
#include <vector>

namespace t2p {
// Only empty container capacity is reused. All decoded values/flags still start
// in a fresh GxState, and consumed events retain no producer-visible references.
class StateVectorPool3ds {
  struct Item { MaterialVector<TevStage> stages; MaterialVector<VtxTexGen> texgen; };
  std::vector<Item> items_;
  size_t bytes_=0;
  static size_t cost(const Item& x) {
    return x.stages.capacity()*sizeof(TevStage)+x.texgen.capacity()*sizeof(VtxTexGen);
  }
 public:
  static constexpr size_t budget=512u<<10, maxItems=1024;
  bool take(GxState& s) {
    if(items_.empty())return false;
    auto& x=items_.back();bytes_-=cost(x);
    s.draw.tev.stages=std::move(x.stages);s.draw.vtx.texgen=std::move(x.texgen);
    items_.pop_back();return true;
  }
  bool give(GxState& s) {
    if(sharedMaterialVector(s.draw.tev.stages)||sharedMaterialVector(s.draw.vtx.texgen))return false;
    const size_t n=s.draw.tev.stages.capacity()*sizeof(TevStage)+s.draw.vtx.texgen.capacity()*sizeof(VtxTexGen);
    if(!n||items_.size()>=maxItems||n>budget-bytes_)return false;
    s.draw.tev.stages.clear();s.draw.vtx.texgen.clear();
    items_.push_back({std::move(s.draw.tev.stages),std::move(s.draw.vtx.texgen)});
    bytes_+=n;return true;
  }
  size_t bytes() const{return bytes_;}
  size_t size() const{return items_.size();}
};
}
