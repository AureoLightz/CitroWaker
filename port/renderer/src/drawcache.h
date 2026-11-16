// Memo for compileDraw. Compiling a draw (TEV search, vertex shader text + its model assembly, fixed-function state, fog LUT) costs ~0.3 ms on a PC and far more on the 3DS CPU, and a
// game draws the same few hundred materials every frame, so the result is kept: the key is the complete input (GxDrawState + Options), compared field by field after a 64 bit hash
// picked the bucket (a hash collision can never return another draw's program). Packets share the cached PicaDraw (DrawPacket::pica()).
//
// The state is hashed through one function (`feed`) that lists every field; tests/test_drawcache.cpp checks that changing any field changes the key (it flips every member of every
// state struct in turn), so a field added to GxDrawState but forgotten here is caught as soon as the test knows about it (the test fails on a byte-level difference in the plain
// sub-structs and lists the vector members explicitly).
#pragma once
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "draw.h"

namespace t2p {

uint64_t hashDrawKey(const GxDrawState&, const Options&);
void replayDrawKey(const GxDrawState&, uint64_t out[2]);   // (Claude v121) 128-bit, pose counts included
bool sameDrawKey(const GxDrawState& a, const Options& oa, const GxDrawState& b, const Options& ob);

// Matrix grouping only: same draw key except the two position-palette counts.
// Frame options are shared by the entire group; every other field is compared.
bool sameDrawKeyIgnoringPoseCount(const GxDrawState&, const GxDrawState&);

struct DrawCacheStats {
  uint64_t lookups = 0, hits = 0, misses = 0, evictions = 0;
  size_t entries = 0;
};

class DrawCompileCache {
 public:
  // hashMask: tests only (0 puts every key in one bucket, to exercise the field-wise comparison that must save the day on a hash collision)
  explicit DrawCompileCache(size_t maxEntries = 4096, uint64_t hashMask = ~0ull) : max_(maxEntries ? maxEntries : 1), mask_(hashMask) {}
  // The compiled draw for this input (compiled on the first request). The pointer stays valid as long as the caller holds it, even when the entry is evicted.
  std::shared_ptr<const PicaDraw> get(const GxDrawState& g, const Options& opt);
  void clear();
  void setRecentKeyFastPath(bool enabled) { recentKeyFastPath_ = enabled; }
  DrawCacheStats stats() const { DrawCacheStats s = st_; s.entries = lru_.size(); return s; }

 private:
  struct Entry {
    uint64_t hash = 0;
    GxDrawState key;
    Options opt;
    std::shared_ptr<const PicaDraw> value;
  };
  bool recentKeyFastPath_ = true;
  size_t max_;
  uint64_t mask_;
  std::list<Entry> lru_;                                          // most recently used first
  std::unordered_multimap<uint64_t, std::list<Entry>::iterator> byHash_;
  DrawCacheStats st_;
};

}  // namespace t2p
