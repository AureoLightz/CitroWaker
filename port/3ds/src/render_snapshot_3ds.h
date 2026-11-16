#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// Bounded texture-only snapshot. Array decoding is producer-fenced separately.
class RenderSnapshot3ds {
  struct Range { uint32_t addr, size, offset; };
  std::vector<Range> ranges;
  // (Claude v102) ranges not copied because the renderer's texture cache already holds them with the same sample hash (texKnownContains)
  struct Known { uint32_t addr, size; uint64_t hash; };
  std::vector<Known> known;
  uint32_t knownBytes = 0;
  uint8_t* storage = nullptr;
  uint32_t capacity = 0, used = 0;
  bool complete = true, frozen = false;
public:
  ~RenderSnapshot3ds() { std::free(storage); }
  bool init(uint32_t bytes) {
    storage = static_cast<uint8_t*>(std::malloc(bytes));
    capacity = storage ? bytes : 0;
    ranges.reserve(256);
    return storage;
  }
  void add(uint32_t addr, uint32_t bytes) {
    if (!addr || !bytes || uint64_t(addr) + bytes > 0x100000000ull) { complete = false; return; }
    for (auto& r : ranges) if (r.addr == addr) { r.size = std::max(r.size,bytes); return; }
    if (ranges.size() == 1024) { complete = false; return; }
    ranges.push_back({addr,bytes,0});
  }
  template<class Reader> bool freeze(Reader read) { return freeze(read, [](uint32_t, uint32_t, const uint8_t*, uint64_t&) { return false; }); }
  // skip(addr, size, liveBytes, hash&): true = the renderer knows these bytes (hash filled), the range is not copied
  template<class Reader, class Skip> bool freeze(Reader read, Skip skip) {
    used = 0; known.clear(); knownBytes = 0;
    if (!complete || !storage) return false;
    if (known.capacity() < 256) known.reserve(256);
    {
      size_t keep = 0;
      for (const Range& r : ranges) {
        uint64_t h = 0; const uint8_t* live = read(r.addr, r.size);
        if (live && skip(r.addr, r.size, live, h)) { known.push_back({r.addr, r.size, h}); knownBytes += r.size; }
        else ranges[keep++] = r;
      }
      ranges.resize(keep);
      std::sort(known.begin(), known.end(), [](const Known& a, const Known& b) { return a.addr < b.addr || (a.addr == b.addr && a.size < b.size); });
    }
    std::sort(ranges.begin(),ranges.end(),[](const Range& a,const Range& b){return a.addr<b.addr;});
    size_t count = 0;
    for (const auto r : ranges) {
      if (count && uint64_t(ranges[count-1].addr)+ranges[count-1].size >= r.addr) {
        auto& prev = ranges[count-1];
        const uint64_t end = std::max(uint64_t(prev.addr)+prev.size,uint64_t(r.addr)+r.size);
        if (end-prev.addr > UINT32_MAX) return false;
        prev.size = uint32_t(end-prev.addr);
      } else ranges[count++] = r;
    }
    ranges.resize(count);
    for (auto& r : ranges) {
      if (r.size > capacity-used) return false;
      const uint8_t* source = read(r.addr,r.size);
      if (!source) return false;
      r.offset = used;
      std::memcpy(storage+used,source,r.size);
      used += r.size;
    }
    frozen = true;
    return true;
  }
  const uint8_t* read(uint32_t addr, size_t size) const {
    if (!frozen) return nullptr;
    // freeze() sorts and merges ranges. Texture lookups need only the last
    // range starting at/before addr, rather than scanning the whole snapshot.
    auto it = std::upper_bound(ranges.begin(), ranges.end(), addr,
      [](uint32_t a, const Range& r) { return a < r.addr; });
    if (it == ranges.begin()) return nullptr;
    const auto& r = *--it;
    if (uint64_t(addr)+size <= uint64_t(r.addr)+r.size)
      return storage+r.offset+(addr-r.addr);
    return nullptr;
  }
  bool hint(uint32_t addr, size_t size, uint64_t& hash) const {
    if (!frozen || known.empty()) return false;
    auto it = std::lower_bound(known.begin(), known.end(), addr, [](const Known& k, uint32_t a) { return k.addr < a; });
    for (; it != known.end() && it->addr == addr; ++it) if (it->size == size) { hash = it->hash; return true; }
    return false;
  }
  // a skipped range may be read from live memory (its bytes matched the cache when frozen; only a cache eviction leads here)
  bool isKnown(uint32_t addr, size_t size) const {
    if (!frozen || known.empty()) return false;
    auto it = std::upper_bound(known.begin(), known.end(), addr, [](uint32_t a, const Known& k) { return a < k.addr; });
    while (it != known.begin()) { --it; if (uint64_t(addr) + size <= uint64_t(it->addr) + it->size) return true; if (addr - it->addr > (4u << 20)) break; }
    return false;
  }
  template<class F> void forEachCopied(F f) const { for (const auto& r : ranges) f(r.addr, r.size); }   // (Claude v116 diag)
  uint32_t skippedBytes() const { return knownBytes; }
  uint32_t bytes() const { return used; }
  void reset() { frozen=false; complete=true; used=0; ranges.clear(); known.clear(); knownBytes=0; }
};
