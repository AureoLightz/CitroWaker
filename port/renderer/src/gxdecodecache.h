#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace t2p {
struct GxAttrPlan;
struct GxDecodeCacheStats {
  uint64_t lookups=0,hits=0,vertices=0,hitVertices=0,changed=0,admitted=0,bypassed=0;
  size_t bytes=0,entries=0;
};

// Exact per-primitive decoded attribute reuse. Every hit validates the command,
// binding metadata and all referenced source bytes. No GPU/guest pointers escape
// into cached output. Ineligible/animated data uses the ordinary decoder.
class GxDecodeCache {
 public:
  explicit GxDecodeCache(size_t budget=3u<<20):budget_(budget){byHash_.reserve(1024);}
  bool lookup(const uint32_t format[5],const uint8_t* stream,uint32_t bytes,
              const GxAttrPlan* attrs,int count,uint32_t vertices,uint32_t vsize,uint64_t clock);
  void store(const uint32_t format[5],const uint8_t* stream,uint32_t bytes,
             const GxAttrPlan* attrs,int count,uint32_t vertices,uint32_t vsize);
  GxDecodeCacheStats stats() const {auto s=stats_;s.bytes=used_;s.entries=entries_.size();return s;}
 private:
  struct Binding {const uint8_t* base=nullptr;uint32_t stride=0,size=0;bool be=true;};
  struct Span {const uint8_t* address;uint32_t bytes,offset;};
  struct Entry {
    uint32_t hash=0,format[5]={},vertices=0;
    std::vector<Binding> bindings;
    std::vector<uint8_t> stream,source,decoded;
    std::vector<Span> spans;
    uint64_t lastUse=0;size_t cost=0;
  };
  struct Cooldown {uint32_t hash=0;uint64_t until=0;};
  using Iterator=std::list<Entry>::iterator;
  size_t budget_,used_=0;
  std::list<Entry> entries_;
  std::unordered_multimap<uint32_t,Iterator> byHash_;
  std::array<Cooldown,1024> cooldown_{};
  GxDecodeCacheStats stats_;
  bool pending_=false;uint32_t pendingHash_=0;uint64_t clock_=0;
  void erase(Iterator it);
};
} // namespace t2p
