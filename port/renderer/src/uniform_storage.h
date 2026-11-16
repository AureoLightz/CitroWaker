#pragma once
#include "vtx.h"
#include "uniform_pieces.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
namespace t2p {
// (Claude v86) Pool for the shared 2 KB vertex-uniform blocks. The producer makes one per state change (and drops it again when it equals the last); the
// renderer frees them as events are recycled and makes one per matrix group. Through newlib each is a locked malloc/free of a large block on cores 2/3
// (console [locks]: ~1-2 thousand per frame on the render thread); here it is a short list push/pop. Only the one block size the pool first sees is
// kept, at most kCap blocks; trim() hands every kept block back (gpu_3ds.cpp calls it when the host heap runs short).
struct FixedBlockPool {
  struct Node { Node* next; };
  explicit FixedBlockPool(unsigned c, size_t sz = 0) : cap(c), size(sz) {}
  const unsigned cap;
  std::mutex mu; Node* head = nullptr; unsigned count = 0; size_t size;   // size 0: the first request's size
  unsigned long long hits = 0, misses = 0;
  void* take(size_t n) {
    { std::lock_guard<std::mutex> g(mu);
      if (!size) size = n;
      if (n == size && head) { Node* x = head; head = x->next; --count; ++hits; return x; }
      if (n == size) ++misses; }
    return ::operator new(n);
  }
  void give(void* p, size_t n) {
    { std::lock_guard<std::mutex> g(mu);
      if (n == size && count < cap) { Node* x = static_cast<Node*>(p); x->next = head; head = x; ++count; return; } }
    ::operator delete(p);
  }
  void trim() { trimTo(0); }
  // (Claude v102) keeps `keep` blocks: the heap fence used to empty the pools every outdoor frame on the console (heap just under its
  // threshold), so every event / uniform block became a locked malloc on one core and a locked free on another (events pool: 0 hits).
  void trimTo(unsigned keep) {
    Node* list = nullptr;
    { std::lock_guard<std::mutex> g(mu);
      while (count > keep && head) { Node* x = head; head = x->next; x->next = list; list = x; --count; } }
    while (list) { Node* x = list; list = x->next; ::operator delete(x); }
  }
};
// Pools are never destroyed: blocks can outlive static destruction.
struct UniformBlockPool { static FixedBlockPool& get() { static FixedBlockPool* p = new FixedBlockPool(256); return *p; } };
template <class T, class Pool> struct PoolAlloc {
  using value_type = T;
  PoolAlloc() = default;
  template <class U> PoolAlloc(const PoolAlloc<U, Pool>&) {}
  template <class U> struct rebind { using other = PoolAlloc<U, Pool>; };
  T* allocate(size_t n) { return static_cast<T*>(Pool::get().take(n * sizeof(T))); }
  void deallocate(T* p, size_t n) { Pool::get().give(p, n * sizeof(T)); }
  template <class U> bool operator==(const PoolAlloc<U, Pool>&) const { return true; }
  template <class U> bool operator!=(const PoolAlloc<U, Pool>&) const { return false; }
};
template <class T> using UniformBlockAlloc = PoolAlloc<T, UniformBlockPool>;
// (Claude v112) the per-draw compact pose blocks (one per GX draw whose matrices changed): pooled like the flat blocks, not a locked malloc each
struct CompactBlockPool { static FixedBlockPool& get() { static FixedBlockPool* p = new FixedBlockPool(512); return *p; } };
template <class... A> std::shared_ptr<VtxUniforms> makeUniformBlock(A&&... a) { return std::allocate_shared<VtxUniforms>(UniformBlockAlloc<VtxUniforms>(), std::forward<A>(a)...); }
// Immutable non-pose rows repeat across a model's material draws. A compact
// recorded pose expands only when a consumer needs the original flat contract.
struct UniformNonPose {
 float proj[4][4],matColor[4],ambColor[4];VtxLight light[8];float texMtx[10][3][4],texScale[3][2];
 explicit UniformNonPose(const VtxUniforms& u){
  std::memcpy(proj,u.proj,sizeof proj);std::memcpy(matColor,u.matColor,sizeof matColor);std::memcpy(ambColor,u.ambColor,sizeof ambColor);
  std::memcpy(light,u.light,sizeof light);std::memcpy(texMtx,u.texMtx,sizeof texMtx);std::memcpy(texScale,u.texScale,sizeof texScale);
 }
};
static_assert(sizeof(UniformNonPose)==1112,"float rows without padding");
// Only the rows this already-compiled shader consumes establish equivalence.
// Unused XF texture matrices/lights can change between models without changing
// any packed uniform. Compare bits; no hashes or float tolerance authorize reuse.
inline bool sameUsedNonPose(const VtxSetup& setup,const VtxShader& shader,const UniformNonPose& a,const UniformNonPose& b){
 if(std::memcmp(a.proj,b.proj,sizeof a.proj)||std::memcmp(a.matColor,b.matColor,sizeof a.matColor)||
    std::memcmp(a.ambColor,b.ambColor,sizeof a.ambColor)||std::memcmp(a.texScale,b.texScale,sizeof a.texScale))return false;
 for(int i=0;i<8;++i)if(shader.lightSlot[i]>=0&&std::memcmp(&a.light[i],&b.light[i],sizeof a.light[i]))return false;
 for(size_t i=0;i<setup.texgen.size()&&i<3;++i)if(shader.texMtxSlot[i]>=0){
   const int k=(setup.texgen[i].mtx-30)/3;if(k<0||k>=10||std::memcmp(a.texMtx[k],b.texMtx[k],sizeof a.texMtx[k]))return false;
 }
 return true;
}

struct CompactUniforms {
 const float (*pos)[3][4]=nullptr;const float (*nrm)[3][4]=nullptr;std::shared_ptr<const UniformNonPose> shared;
 std::shared_ptr<const UniformPieces> pieces;
 // (Claude v112) pose rows [0, npose) are filled; the rest are not (a draw's shader reads slots below its numPosMtx only: matrixbatch, frame
 // and vtx index pose k < numPosMtx). A full block (npose 10) comes from the VtxUniforms constructor.
 int npose=10;
 mutable std::atomic<const VtxUniforms*> published{nullptr};
 ~CompactUniforms(){delete published.load(std::memory_order_relaxed);}
 // the pose rows a draw with `n` position matrices reads, straight from XF memory (selected != 0: that matrix in slot 0, n is then 1)
 static void poseRows(float (*pos)[3][4],float (*nrm)[3][4],const uint32_t* xf,int selected,int n){
  const int from=selected?selected:0;
  std::memcpy(pos,xf+from*12,size_t(n)*sizeof pos[0]);
  const uint32_t* xw=xf+0x400+from*9;uint32_t* nw=reinterpret_cast<uint32_t*>(&nrm[0][0][0]);
  for(int row=0;row<3*n;++row,nw+=4,xw+=3){nw[0]=xw[0];nw[1]=xw[1];nw[2]=xw[2];nw[3]=0;}
 }
 CompactUniforms(int n,std::shared_ptr<const UniformNonPose> p):shared(std::move(p)),npose(n){}
 CompactUniforms(int n,std::shared_ptr<const UniformPieces> p):pieces(std::move(p)),npose(n){}
 // equal to what the constructor above would make from these registers (rows [0, n) only: a block with npose >= n serves the draw)
 bool samePose(const uint32_t* xf,int selected,int n)const{
  if(npose<n)return false;
  const int from=selected?selected:0;
  if(std::memcmp(pos,xf+from*12,size_t(n)*sizeof pos[0]))return false;
  const uint32_t* xw=xf+0x400+from*9;const uint32_t* nw=reinterpret_cast<const uint32_t*>(&nrm[0][0][0]);
  for(int row=0;row<3*n;++row,nw+=4,xw+=3)if(nw[0]!=xw[0]||nw[1]!=xw[1]||nw[2]!=xw[2])return false;
  return true;
 }
 // serves a draw reading pose rows [0, n) whose uniforms are u
 bool matches(const VtxUniforms& u,int n)const{
  if(npose<n||std::memcmp(pos,u.pos,size_t(n)*sizeof pos[0])||std::memcmp(nrm,u.nrm,size_t(n)*sizeof nrm[0]))return false;
  if(pieces){const auto v=view();return !std::memcmp(v.proj,u.proj,sizeof u.proj)&&!std::memcmp(v.matColor,u.matColor,sizeof u.matColor)&&!std::memcmp(v.ambColor,u.ambColor,sizeof u.ambColor)&&!std::memcmp(v.light,u.light,sizeof u.light)&&!std::memcmp(v.texMtx,u.texMtx,sizeof u.texMtx)&&!std::memcmp(v.texScale,u.texScale,sizeof u.texScale);}
  const UniformNonPose p(u);return !std::memcmp(shared.get(),&p,sizeof p);
 }
 VtxUniformView view()const{if(pieces)return pieces->view(pos,nrm,npose);return {shared->proj,pos,nrm,shared->matColor,shared->ambColor,shared->light,shared->texMtx,shared->texScale,npose};}
 const VtxUniforms& get()const{
  if(const auto* p=published.load(std::memory_order_acquire))return *p;
  auto u=std::make_unique<VtxUniforms>();
  std::memcpy(u->pos,pos,size_t(npose)*sizeof pos[0]);std::memcpy(u->nrm,nrm,size_t(npose)*sizeof nrm[0]);
  const auto v=view();std::memcpy(u->proj,v.proj,sizeof u->proj);std::memcpy(u->matColor,v.matColor,sizeof u->matColor);std::memcpy(u->ambColor,v.ambColor,sizeof u->ambColor);
  std::memcpy(u->light,v.light,sizeof u->light);std::memcpy(u->texMtx,v.texMtx,sizeof u->texMtx);std::memcpy(u->texScale,v.texScale,sizeof u->texScale);
  // Racing first readers may build the same rows, but never wait on a lower
  // priority thread. Only the CAS winner transfers ownership to this object.
  const VtxUniforms* expected=nullptr;
  if(published.compare_exchange_strong(expected,u.get(),std::memory_order_acq_rel,std::memory_order_acquire))return *u.release();
  return *expected;
 }
};
// Derived storage keeps the control block and precisely sized rows in ONE
// allocation. Converting the shared_ptr preserves the derived deleter; no
// borrowed pointers or extra per-record heap allocations cross the frame fence.
template<int N> struct CompactUniformRows final : CompactUniforms {
 float storage[2][N][3][4];
 CompactUniformRows(const uint32_t* xf,int selected,int n,std::shared_ptr<const UniformNonPose> p):CompactUniforms(n,std::move(p)){
  pos=storage[0];nrm=storage[1];poseRows(storage[0],storage[1],xf,selected,n);
 }
 CompactUniformRows(const uint32_t* xf,int selected,int n,std::shared_ptr<const UniformPieces> p):CompactUniforms(n,std::move(p)){
  pos=storage[0];nrm=storage[1];poseRows(storage[0],storage[1],xf,selected,n);
 }
 CompactUniformRows(const VtxUniforms& u,int n,std::shared_ptr<const UniformNonPose> p):CompactUniforms(n,std::move(p)){
  pos=storage[0];nrm=storage[1];std::memcpy(storage[0],u.pos,size_t(n)*sizeof storage[0][0]);std::memcpy(storage[1],u.nrm,size_t(n)*sizeof storage[1][0]);
 }
};
extern bool g_compactPoseStorage;
template<class... Args>
std::shared_ptr<const CompactUniforms> allocateCompactUniforms(int n,Args&&... args){
 if(n<1||n>10)throw std::bad_alloc();
 const int storage=g_compactPoseStorage?n:10;
 #define CPOSE_CASE(N) case N:return std::allocate_shared<const CompactUniformRows<N>>(PoolAlloc<CompactUniformRows<N>,CompactBlockPool>(),std::forward<Args>(args)...)
 switch(storage){CPOSE_CASE(1);CPOSE_CASE(2);CPOSE_CASE(3);CPOSE_CASE(4);CPOSE_CASE(5);CPOSE_CASE(6);CPOSE_CASE(7);CPOSE_CASE(8);CPOSE_CASE(9);CPOSE_CASE(10);}
 #undef CPOSE_CASE
 throw std::bad_alloc();
}
inline std::shared_ptr<const CompactUniforms> makeCompactUniforms(const VtxUniforms& u,int n,std::shared_ptr<const UniformNonPose> p){return allocateCompactUniforms(n,u,n,std::move(p));}
inline std::shared_ptr<const CompactUniforms> makeCompactUniforms(const uint32_t* xf,int selected,int n,std::shared_ptr<const UniformNonPose> p){return allocateCompactUniforms(n,xf,selected,n,std::move(p));}
inline std::shared_ptr<const CompactUniforms> makeCompactPieceUniforms(const uint32_t* xf,int selected,int n,std::shared_ptr<const UniformPieces> p){return allocateCompactUniforms(n,xf,selected,n,std::move(p));}
extern bool g_uniformSections; // configured before producer/renderer startup
std::shared_ptr<const CompactUniforms> compactUniforms(const VtxUniforms&,int npose=10);
}
