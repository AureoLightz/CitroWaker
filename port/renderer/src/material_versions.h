#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <limits>
namespace t2p {
struct MaterialVersion {
 uintptr_t source=0;uint32_t size=0,lifetime=0,global=0,page[2]={};
 bool operator==(const MaterialVersion&)const=default;
 bool valid()const{return source&&size&&size<=2048&&global!=UINT32_MAX&&page[0]!=UINT32_MAX&&page[1]!=UINT32_MAX;}
};
// GDInitGDLObj opens every material-DL write in the current J3D implementation,
// including external texture/index patches. Collisions invalidate extra lists;
// they can never preserve a changed list. Saturation permanently disables hits
// rather than allowing a counter wrap to resurrect an old program.
class MaterialVersions {
 static constexpr size_t kPages=4096;
 std::array<std::atomic<uint32_t>,kPages> pages{};
 std::atomic<uint32_t> global{0};
 static size_t index(uintptr_t page){page^=page>>12;return page&(kPages-1);}
 static void bump(std::atomic<uint32_t>& word){auto old=word.load(std::memory_order_relaxed);while(old!=UINT32_MAX&&!word.compare_exchange_weak(old,old+1,std::memory_order_release,std::memory_order_relaxed)){} }
public:
 void writing(const void* source,size_t size){
  const auto at=reinterpret_cast<uintptr_t>(source);
  if(!at||!size)return;
  if(size>262144||at>UINTPTR_MAX-(size-1)){bump(global);return;}
  const auto first=at>>12,last=(at+size-1)>>12;
  for(auto page=first;page<=last;++page)bump(pages[index(page)]);
 }
 MaterialVersion sample(const void* source,size_t size,uint32_t lifetime)const{
  MaterialVersion v;const auto at=reinterpret_cast<uintptr_t>(source);
  if(!at||!size||size>2048||at>UINTPTR_MAX-(size-1))return v;
  v.source=at;v.size=uint32_t(size);v.lifetime=lifetime;v.global=global.load(std::memory_order_acquire);
  v.page[0]=pages[index(at>>12)].load(std::memory_order_acquire);v.page[1]=pages[index((at+size-1)>>12)].load(std::memory_order_acquire);return v;
 }
};
}
