#pragma once
#include "gxstate.h"
#include "raw_uniform_recipe.h"
#include "direct_group_rows.h"
#include <atomic>
#include <cassert>
#include <cmath>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
namespace t2p {
// The complete static inputs used by decodeState, with pose/light/colour data
// deliberately excluded: those go straight through the compiled uniform recipe.
// Projection/viewport/scissor/texture identity stay exact in the initial path.
struct DrawRegisterSignature {
 std::array<uint32_t,150> bp{};
 std::array<uint32_t,34> controls{};
 uint32_t colors[2][8]={},textures[8]={};uint16_t written=0;bool aurora=false,dynamicProjection=false;
 static bool bpUsed(unsigned i){return stateDirtyBp(i)!=0;}
 static bool xfUsed(unsigned i){return i==0x0e||i==0x10||i==0x12||i==0x18||i==0x19||(i>=0x1a&&i<=0x26)||(i>=0x40&&i<=0x47)||(i>=0x50&&i<=0x57);}
 static const std::vector<uint16_t>& bpIndices(){static const auto ids=[](){std::vector<uint16_t> v;for(unsigned i=0;i<256;++i)if(bpUsed(i))v.push_back(uint16_t(i));return v;}();return ids;}
 static const std::vector<uint16_t>& xfIndices(){static const auto ids=[](){std::vector<uint16_t> v;for(unsigned i=0;i<256;++i)if(xfUsed(i))v.push_back(uint16_t(i));return v;}();return ids;}
 static uint32_t xfWord(const GxRegs& r,unsigned i){return i==0x18?r.xf[0x1000+i]&~63u:r.xf[0x1000+i];}
 static uint16_t writtenBits(const GxRegs& r){uint16_t bits=r.bpWritten[0x59]?1:0;for(unsigned i=0;i<8;++i)if(r.bpWritten[0x88+(i<4?i:i+28)])bits|=uint16_t(2u<<i);return bits;}
 explicit DrawRegisterSignature(const GxRegs& r,bool camera=false):dynamicProjection(camera){
  for(unsigned i=0x20;i<0x26;++i){float value;std::memcpy(&value,&r.xf[0x1000+i],4);if(!std::isfinite(value))dynamicProjection=false;}
  size_t at=0;assert(bpIndices().size()==bp.size()&&xfIndices().size()==controls.size());
  for(auto i:bpIndices())bp[at++]=r.bp[i];at=0;for(auto i:xfIndices())controls[at++]=xfWord(r,i);
  std::memcpy(colors,r.tevRaw,sizeof colors);std::memcpy(textures,r.texAddr,sizeof textures);written=writtenBits(r);aurora=r.auroraViewport;
 }
 // Diagnostic only; reports which conservative guard blocked a known program.
 unsigned mismatchClass(const GxRegs& r)const{
  if(written!=writtenBits(r)||aurora!=r.auroraViewport)return 1;
  if(std::memcmp(colors,r.tevRaw,sizeof colors))return 2;
  if(std::memcmp(textures,r.texAddr,sizeof textures))return 3;
  size_t at=0;for(auto i:bpIndices())if(bp[at++]!=r.bp[i])return 4;
  at=0;for(auto i:xfIndices()){auto expected=controls[at++];if(dynamicProjection&&i>=0x20&&i<0x24){float f;std::memcpy(&f,&r.xf[0x1000+i],4);if(!std::isfinite(f))return 5;continue;}if(expected!=xfWord(r,i))return 5;}
  return 0;
 }
 bool matches(const GxRegs& r)const{
  if(written!=writtenBits(r)||aurora!=r.auroraViewport||std::memcmp(colors,r.tevRaw,sizeof colors)||std::memcmp(textures,r.texAddr,sizeof textures))return false;
  size_t at=0;for(auto i:bpIndices())if(bp[at++]!=r.bp[i])return false;at=0;
  for(auto i:xfIndices()){const uint32_t expected=controls[at++];
   if(dynamicProjection&&i>=0x20&&i<0x24){float value;std::memcpy(&value,&r.xf[0x1000+i],4);if(!std::isfinite(value))return false;continue;}
   if(expected!=xfWord(r,i))return false;
  }
  return true;
 }
};
struct RetainedGeometry;
struct DirectGroupBinding;
struct DirectDrawBinding {
 std::weak_ptr<const DirectGroupBinding> group;uint16_t groupRole=0;
 std::weak_ptr<const RetainedGeometry> geometryOwner;
 uint64_t geometry=0;
 std::shared_ptr<const DrawRegisterSignature> signature;
 GxState state; // immutable static/frame-policy view, never a source of poses
 std::shared_ptr<const PicaDraw> compiled;
 std::shared_ptr<const UniformRecipe> recipe;
 std::vector<UniformRecipe::Range> ranges;
 std::weak_ptr<void> executor;
 float projection[4][4]={},post[4][4]={},scale[3][2]={};
 explicit DirectDrawBinding(const GxRegs& r):signature(std::make_shared<DrawRegisterSignature>(r)){}
 explicit DirectDrawBinding(std::shared_ptr<const DrawRegisterSignature> s):signature(std::move(s)){}
};
struct DirectGroupBinding {
 std::vector<std::shared_ptr<const DirectDrawBinding>> members;
 DirectGroupRows rows;
 std::shared_ptr<const UniformRecipe> recipe;
 std::weak_ptr<void> executor;
 size_t bytes=0;
};
class DirectGroupRegistry {
public:
 struct Snapshot {std::array<std::shared_ptr<const DirectGroupBinding>,64> entries;};
 using View=std::shared_ptr<const Snapshot>;
private:
 View published_;
 std::array<std::shared_ptr<const DirectGroupBinding>,64> entries_{};
 size_t bytes_=0;
public:
 View readView()const{return std::atomic_load_explicit(&published_,std::memory_order_acquire);}
 void publishFrame(){try{View view=std::make_shared<Snapshot>(Snapshot{entries_});std::atomic_store_explicit(&published_,std::move(view),std::memory_order_release);}catch(const std::bad_alloc&){}}
 std::shared_ptr<const DirectGroupBinding> find(uint64_t geometry,const std::shared_ptr<const RetainedGeometry>& owner,const GxRegs& regs,const std::shared_ptr<const DrawRegisterSignature>& proven={},const View& view={})const{
  if(view){const auto first=(geometry&15)*4;for(size_t i=first;i<first+4;++i){const auto& p=view->entries[i];if(p&&!p->members.empty()&&!p->executor.expired()){const auto& root=p->members[0];if(root->geometry==geometry&&root->geometryOwner.lock()==owner&&(root->signature==proven||root->signature->matches(regs)))return p;}}return {};}
  const auto first=(geometry&15)*4;for(size_t i=first;i<first+4;++i){auto p=std::atomic_load_explicit(&entries_[i],std::memory_order_acquire);if(p&&!p->members.empty()&&!p->executor.expired()){const auto& root=p->members[0];if(root->geometry==geometry&&root->geometryOwner.lock()==owner&&(root->signature==proven||root->signature->matches(regs)))return p;}}return {};
 }
 unsigned diagnose(uint64_t id,const std::shared_ptr<const RetainedGeometry>& owner,const GxRegs& r)const{
  const auto first=(id&15)*4;for(size_t i=first;i<first+4;++i){auto p=std::atomic_load_explicit(&entries_[i],std::memory_order_acquire);if(p&&!p->members.empty()){auto& b=p->members[0];if(b->geometry==id&&b->geometryOwner.lock()==owner&&!p->executor.expired())return 1+b->signature->mismatchClass(r);}}return 0;
 }
 bool publish(std::shared_ptr<const DirectGroupBinding> p){
  if(!p||p->members.empty()||p->bytes>(2u<<20))return false;
  const auto first=(p->members[0]->geometry&15)*4;
  for(size_t i=first;i<first+4;++i){auto old=std::atomic_load_explicit(&entries_[i],std::memory_order_acquire);if(!old||old->executor.expired()){
    const auto oldBytes=old?old->bytes:0;if(bytes_-oldBytes+p->bytes>(2u<<20))return false;
    bytes_=bytes_-oldBytes+p->bytes;std::atomic_store_explicit(&entries_[i],std::move(p),std::memory_order_release);return true;
  }}return false;
 }
};
// Only the perspective X/Y coefficients may vary. Depth-plane coefficients,
// viewport, scissor and projection type still match exactly. Folded depth rows
// stay fixed; recompute the first two rows in mapFrame's original double order.
inline void directProjection(const DirectDrawBinding& binding,const uint32_t* xf,float output[4][4]){
 std::memcpy(output,binding.projection,sizeof(binding.projection));
 if(!binding.signature->dynamicProjection)return;
 struct Memo{bool valid=false;uint32_t key[20]={};float rows[2][4]={};};
 static thread_local Memo memo;
 uint32_t key[20];std::memcpy(key,xf+0x1020,16);std::memcpy(key+4,binding.post,32);std::memcpy(key+12,binding.projection[2],32);
 if(memo.valid&&!std::memcmp(key,memo.key,sizeof key)){std::memcpy(output,memo.rows,sizeof memo.rows);return;}
 float source[4][4];std::memcpy(source,binding.state.proj.m,sizeof source);
 std::memcpy(&source[0][0],xf+0x1020,4);std::memcpy(&source[0][2],xf+0x1021,4);
 std::memcpy(&source[1][1],xf+0x1022,4);std::memcpy(&source[1][2],xf+0x1023,4);
 for(int row=0;row<2;++row)for(int col=0;col<4;++col){double sum=0;for(int k=0;k<4;++k)sum+=double(binding.post[row][k])*double(source[k][col]);output[row][col]=float(sum);}
 std::memcpy(memo.key,key,sizeof key);std::memcpy(memo.rows,output,sizeof memo.rows);memo.valid=true;
}
// Bounded immutable publication. Readers never observe a half-built record and
// frame-owned references keep its source description alive after replacement.
class DirectDrawRegistry {
 struct Interest {uint64_t geometry;std::weak_ptr<const RetainedGeometry> owner;};
public:
 struct Snapshot {std::array<std::shared_ptr<const DirectDrawBinding>,512> slots;std::array<std::shared_ptr<const Interest>,512> interests;};
 using View=std::shared_ptr<const Snapshot>;
private:
 View published_;
 std::array<std::shared_ptr<const DirectDrawBinding>,512> slots_{};

 std::array<std::shared_ptr<const Interest>,512> interests_{};
 size_t activeSlots_=128;
public:
 View readView()const{return std::atomic_load_explicit(&published_,std::memory_order_acquire);}
 void publishFrame(){try{View view=std::make_shared<Snapshot>(Snapshot{slots_,interests_});std::atomic_store_explicit(&published_,std::move(view),std::memory_order_release);}catch(const std::bad_alloc&){}}
 void configure(size_t slots){if(slots!=128&&slots!=256&&slots!=512)throw std::invalid_argument("direct slots must be 128/256/512");activeSlots_=slots;}
 bool wanted(uint64_t geometry,const std::shared_ptr<const RetainedGeometry>& owner,const View& view={})const{
  if(view){const size_t first=(geometry&(activeSlots_/4-1))*4;for(size_t i=first;i<first+4;++i){const auto& p=view->interests[i];if(p&&p->geometry==geometry&&p->owner.lock()==owner)return true;}return false;}
  const size_t first=(geometry&(activeSlots_/4-1))*4;for(size_t i=first;i<first+4;++i){auto p=std::atomic_load_explicit(&interests_[i],std::memory_order_acquire);if(p&&p->geometry==geometry&&p->owner.lock()==owner)return true;}return false;
 }
 void request(uint64_t geometry,const std::shared_ptr<const RetainedGeometry>& owner){
  const size_t first=(geometry&(activeSlots_/4-1))*4;size_t empty=512;
  for(size_t i=first;i<first+4;++i){auto p=std::atomic_load_explicit(&interests_[i],std::memory_order_acquire);if(p&&p->geometry==geometry&&p->owner.lock()==owner)return;if((!p||p->owner.expired())&&empty==512)empty=i;}
  // Keep a stable bounded working set instead of allocating signatures for every
  // draw, including matrix groups that this executor cannot consume.
  if(empty!=512){std::shared_ptr<const Interest> p=std::make_shared<Interest>(Interest{geometry,owner});std::atomic_store_explicit(&interests_[empty],std::move(p),std::memory_order_release);}
 }
 std::shared_ptr<const DirectDrawBinding> find(uint64_t geometry,const std::shared_ptr<const RetainedGeometry>& owner,const GxRegs& r,const std::shared_ptr<const DrawRegisterSignature>& proven={},const View& view={})const{
  if(view){const size_t first=(geometry&(activeSlots_/4-1))*4;for(size_t i=first;i<first+4;++i){const auto& p=view->slots[i];if(p&&p->geometry==geometry&&p->geometryOwner.lock()==owner&&(p->signature==proven||p->signature->matches(r))&&!p->executor.expired())return p;}return {};}
  const size_t first=(geometry&(activeSlots_/4-1))*4;
  for(size_t i=first;i<first+4;++i){auto p=std::atomic_load_explicit(&slots_[i],std::memory_order_acquire);if(p&&p->geometry==geometry&&p->geometryOwner.lock()==owner&&(p->signature==proven||p->signature->matches(r))&&!p->executor.expired())return p;}
  return {};
 }
 unsigned diagnose(uint64_t id,const std::shared_ptr<const RetainedGeometry>& owner,const GxRegs& r)const{
  const auto first=(id&(activeSlots_/4-1))*4;for(size_t i=first;i<first+4;++i){auto p=std::atomic_load_explicit(&slots_[i],std::memory_order_acquire);if(p&&p->geometry==id&&p->geometryOwner.lock()==owner&&!p->executor.expired())return 1+p->signature->mismatchClass(r);}return 0;
 }
 bool canLearn(uint64_t geometry,const View& view={})const{
  if(view){const size_t first=(geometry&(activeSlots_/4-1))*4;for(size_t i=first;i<first+4;++i){const auto& p=view->slots[i];if(!p||p->executor.expired()||p->geometryOwner.expired())return true;}return false;}
  const size_t first=(geometry&(activeSlots_/4-1))*4;for(size_t i=first;i<first+4;++i){auto p=std::atomic_load_explicit(&slots_[i],std::memory_order_acquire);if(!p||p->executor.expired()||p->geometryOwner.expired())return true;}return false;
 }
 void publish(std::shared_ptr<const DirectDrawBinding> p){
  const size_t first=(p->geometry&(activeSlots_/4-1))*4;
  for(size_t i=first;i<first+4;++i){auto old=std::atomic_load_explicit(&slots_[i],std::memory_order_acquire);if(!old||old->executor.expired()||old->geometryOwner.expired()){std::atomic_store_explicit(&slots_[i],std::move(p),std::memory_order_release);return;}}
  // A long instance list must not evict all its own programs every frame.
  // Full live buckets retain their exact working set; misses use ordinary GX.
 }
 void clear(){for(auto& p:interests_)std::atomic_store_explicit(&p,std::shared_ptr<const Interest>{},std::memory_order_release);for(auto& p:slots_)std::atomic_store_explicit(&p,std::shared_ptr<const DirectDrawBinding>{},std::memory_order_release);}
};
}

namespace t2p {
struct DirectDrawPayload {
 std::shared_ptr<const DirectDrawBinding> owner;
 uint32_t firstRow=0,rowCount=0;
 std::shared_ptr<const DirectGroupBinding> groupOwner;
 uint32_t projectionXY[4]={},guardColors[2]={};
};
// The legacy state is constructed ONLY for an ordinary draw. A direct record
// holds an immutable program reference plus a range in the frame's row arena;
// it never initializes/copies a multi-kilobyte GxState or creates a uniform block.
// Experimental ABI variant: direct events no longer reserve an inline GxState.
// Build flag defaults off; both producer and consumer must use the same ABI.
#if defined(N3DS_EVENT_INDIRECT) && N3DS_EVENT_INDIRECT
struct DirectLegacyStatePool {static FixedBlockPool& get(){static auto* p=new FixedBlockPool(256,sizeof(GxState));return *p;}};
class DeferredDrawState {
 union Storage {GxState* legacy;DirectDrawPayload direct;Storage(){}~Storage(){}};
 mutable Storage storage_;enum Tag:uint8_t{Empty,Legacy,Direct};mutable Tag tag_=Empty;
 template<class... A>static GxState* create(A&&... args){void* p=DirectLegacyStatePool::get().take(sizeof(GxState));try{return new(p)GxState(std::forward<A>(args)...);}catch(...){DirectLegacyStatePool::get().give(p,sizeof(GxState));throw;}}
 void destroy(){if(tag_==Legacy){storage_.legacy->~GxState();DirectLegacyStatePool::get().give(storage_.legacy,sizeof(GxState));}else if(tag_==Direct)storage_.direct.~DirectDrawPayload();tag_=Empty;}
 void move(DeferredDrawState& other){if(other.tag_==Legacy){storage_.legacy=other.storage_.legacy;tag_=Legacy;other.tag_=Empty;}else if(other.tag_==Direct){new(&storage_.direct)DirectDrawPayload(std::move(other.storage_.direct));tag_=Direct;other.destroy();}}
public:
 DeferredDrawState(){}~DeferredDrawState(){destroy();}
 DeferredDrawState(const DeferredDrawState& other){if(other.tag_==Legacy){storage_.legacy=create(*other.storage_.legacy);tag_=Legacy;}else if(other.tag_==Direct){new(&storage_.direct)DirectDrawPayload(other.storage_.direct);tag_=Direct;}}
 DeferredDrawState& operator=(const DeferredDrawState& other){if(this!=&other){DeferredDrawState copy(other);*this=std::move(copy);}return *this;}
 DeferredDrawState(DeferredDrawState&& other)noexcept{move(other);}
 DeferredDrawState& operator=(DeferredDrawState&& other)noexcept{if(this!=&other){destroy();move(other);}return *this;}
 bool direct()const{return tag_==Direct;}
 GxState& state(){if(tag_==Direct)throw std::logic_error("direct draw requires explicit fallback");if(tag_==Empty){storage_.legacy=create();tag_=Legacy;}return *storage_.legacy;}
 const GxState& state()const{return const_cast<DeferredDrawState*>(this)->state();}
 const GxState& view()const{return tag_==Direct?storage_.direct.owner->state:state();}
 DirectDrawPayload& payload(){assert(tag_==Direct);return storage_.direct;}
 const DirectDrawPayload& payload()const{assert(tag_==Direct);return storage_.direct;}
 void setDirect(std::shared_ptr<const DirectDrawBinding> owner,uint32_t first,uint32_t count){assert(owner);destroy();new(&storage_.direct)DirectDrawPayload{std::move(owner),first,count};tag_=Direct;}
 void reset(){destroy();}
};
#else
class DeferredDrawState {
 union Storage {GxState legacy;DirectDrawPayload direct;Storage(){}~Storage(){}};
 mutable Storage storage_;enum Tag:uint8_t{Empty,Legacy,Direct};mutable Tag tag_=Empty;
 void destroy(){if(tag_==Legacy)storage_.legacy.~GxState();else if(tag_==Direct)storage_.direct.~DirectDrawPayload();tag_=Empty;}
 void move(DeferredDrawState& other){
  if(other.tag_==Legacy){new(&storage_.legacy)GxState(std::move(other.storage_.legacy));tag_=Legacy;}
  else if(other.tag_==Direct){new(&storage_.direct)DirectDrawPayload(std::move(other.storage_.direct));tag_=Direct;}
  other.destroy();
 }
public:
 DeferredDrawState(){}~DeferredDrawState(){destroy();}
 DeferredDrawState(const DeferredDrawState& other){
  if(other.tag_==Legacy){new(&storage_.legacy)GxState(other.storage_.legacy);tag_=Legacy;}
  else if(other.tag_==Direct){new(&storage_.direct)DirectDrawPayload(other.storage_.direct);tag_=Direct;}
 }
 DeferredDrawState& operator=(const DeferredDrawState& other){if(this!=&other){DeferredDrawState copy(other);*this=std::move(copy);}return *this;}
 DeferredDrawState(DeferredDrawState&& other)noexcept(std::is_nothrow_move_constructible_v<GxState>){move(other);}
 DeferredDrawState& operator=(DeferredDrawState&& other)noexcept(std::is_nothrow_move_constructible_v<GxState>){if(this!=&other){destroy();move(other);}return *this;}
 bool direct()const{return tag_==Direct;}
 GxState& state(){if(tag_==Direct)throw std::logic_error("direct draw requires explicit fallback");if(tag_==Empty){new(&storage_.legacy)GxState();tag_=Legacy;}return storage_.legacy;}
 const GxState& state()const{return const_cast<DeferredDrawState*>(this)->state();}
 const GxState& view()const{return tag_==Direct?storage_.direct.owner->state:state();}
 DirectDrawPayload& payload(){assert(tag_==Direct);return storage_.direct;}
 const DirectDrawPayload& payload()const{assert(tag_==Direct);return storage_.direct;}
 void setDirect(std::shared_ptr<const DirectDrawBinding> owner,uint32_t first,uint32_t count){assert(owner);destroy();new(&storage_.direct)DirectDrawPayload{std::move(owner),first,count};tag_=Direct;}
 void reset(){destroy();}
};
#endif
}
