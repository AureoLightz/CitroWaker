// Texture cache: GX texture bindings -> converted PICA textures, shared between draws and frames.
//
// Converting a GX image (decode, fit, transcode, ETC1 search) is expensive; a frame binds the same few hundred images thousands of times. The cache
// maps an image identity to one MappedTexture (texmap.h) and hands out the entry together with the binding's own sampler.
//
// Identity (the key): address, format, size, number of stored mip levels, wrap modes (they decide how non-power-of-two axes are padded), the palette
// (content hash + format, for colour-indexed formats) and whether the TEV reads alpha. Filters, LOD bias and clamps are NOT part of the key: they only
// change the sampler (samplerFor), so one entry serves bindings that differ in them.
//
// Staleness. The game may overwrite texture memory, so an entry is only valid while the image bytes are unchanged:
//   * every lookup compares a cheap sample hash (a few hundred bytes spread over the image) - catches memory reused for another image almost always;
//   * beginFrame() re-hashes whole images round-robin within a byte budget - catches the rest, with a delay of a few frames;
//   * invalidateRange() / invalidateAll() are the exact route for the host to call when it knows (DCFlushRange, GXInvalidateTexRegion, heap free, ...).
// Palettes are snapshotted when the game loads them (TlutMemory, BP 0x65), like the hardware's TMEM, and are hashed on every lookup.
//
// Memory: entries are evicted least-recently-used once the converted bytes exceed the budget, but never within `protectFrames` frames of their last use
// (the GPU may still be reading them). The host learns about evictions through the callback and releases its GPU copy there.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gxstate.h"
#include "texmap.h"

namespace t2p {

class TexPackReader;   // texpack.h
struct TexPackRecord;

// Guest (GameCube physical address space) memory, supplied by the host.
class GuestMem {
 public:
  virtual ~GuestMem() = default;
  virtual const uint8_t* read(uint32_t addr, size_t size) const = 0;   // nullptr when any byte of the range is unmapped
  // (Claude v102) the producer already hashed this range (sampleHashOf, `sampleBytes`) instead of copying it: a cache entry with that hash is a hit
  // without reading the bytes. false: no hint (read() as usual).
  virtual bool hashHint(uint32_t addr, size_t size, uint64_t& hash) const { (void)addr; (void)size; (void)hash; return false; }
};
uint64_t sampleHashOf(const uint8_t* p, size_t n, size_t sampleBytes);   // the cache's validation hash (texcache.cpp)
// (Claude v102) known-texture table: the cache publishes (addr, bytes, sample hash) of its entries after each frame; the producer looks
// ranges up here and skips copying textures the cache already holds with the same bytes. Safe across threads: double-buffered, the
// producer reads the last published copy (the renderer publishes at most once while a producer frame is frozen).
bool texKnownContains(uint32_t addr, uint32_t bytes, uint64_t hash);
bool texRenderedAt(uint32_t addr);   // (Claude v116) an EFB copy destination the cache holds as a GPU texture

// The TLUT half of TMEM: BP 0x65 copies palette entries from main memory; texture units refer to them by TMEM offset.
class TlutMemory {
 public:
  static constexpr uint32_t kUnitBytes = 512;    // unit of GxTlutLoad::tmemOffset and GxTexBinding::tlutOffset (assumed equal; only matters for overlapping loads)
  static constexpr uint32_t kUnits = 1024;       // 10 bit fields
  static constexpr uint32_t kLineBytes = 32;     // load granularity (16 entries)
  TlutMemory();
  TlutMemory(const TlutMemory&) = default;
  TlutMemory& operator=(const TlutMemory& other) {
    if(this!=&other){++generation_;bytes_=other.bytes_;lineLoaded_=other.lineLoaded_;loads_=other.loads_;}
    return *this;
  }
  uint64_t generation() const { return generation_; }
  // Copies the entries from main memory (a load that runs past the area is cut; unreadable memory loads nothing). Returns the number of bytes loaded.
  size_t load(const GxTlutLoad& L, const GuestMem& mem);
  // The `entries` palette entries that start at TMEM offset `offset` (units of kUnitBytes), as `fmt` (GxTlutFmt). Entries nobody loaded read as zero
  // bytes and are counted in `missing`. `hash` identifies the bytes (and the format).
  GxTlut resolve(int fmt, uint32_t offset, int entries, uint64_t& hash, int& missing) const;
  // Same hash / missing count without decoding the palette (what a lookup needs to find its entry).
  uint64_t hashOf(int fmt, uint32_t offset, int entries, int& missing) const;
  uint32_t loads() const { return loads_; }
  // (Claude) raw palette bytes (big-endian entries) from TMEM offset `offset`; `avail` = entries up to the end of the area (texture pack keys)
  const uint8_t* rawAt(uint32_t offset, size_t& avail) const {
    const size_t off = size_t(offset) * kUnitBytes; if (off >= bytes_.size()) { avail = 0; return nullptr; }
    avail = (bytes_.size() - off) / 2; return &bytes_[off];
  }
 private:
  std::vector<uint8_t> bytes_;
  std::vector<uint8_t> lineLoaded_;
  uint32_t loads_ = 0;
  uint64_t generation_ = 0;
};

struct TexCacheOptions {
  bool directEncode=false; // final texture bytes may be written into backend-owned storage

  size_t budgetBytes = 24u << 20;          // converted texture bytes kept (beyond that, least recently used entries go)
  int protectFrames = 2;                   // an entry used this many frames ago or less is never evicted
  size_t verifyBytesPerFrame = 64u << 10;  // guest bytes re-hashed per beginFrame() (0 = never). (Claude v104) 256 -> 64 KB: lookups already compare a sampled hash; this full re-hash is the slow safety net (11% of the forest render core)
  size_t sampleBytes = 512;                // guest bytes hashed per lookup
  bool fastLossyError = false; // exact error LUT and rejection shortcut; experimental
  bool tryEtc1 = true;                     // TexConvOptions::tryEtc1 for every conversion (run time: false, ETC1 encoding is too slow for the ARM11)
  bool memoPalettes = true;                // exact palette hashes reused only until the next load/assignment
  bool sampleOncePerFrame = false;         // guest memory does not change between beginFrame() calls (a renderer replaying a finished frame): an entry whose
                                           // sample hash matched this frame is not sampled again by later lookups in the same frame
};

struct TexCacheStats {
  uint64_t lookups = 0, hits = 0, misses = 0;
  uint64_t conversions = 0;                // mapTexture() calls
  uint64_t staleSample = 0, staleVerify = 0, invalidated = 0, evicted = 0;
  uint64_t retiredHits = 0;   // (Claude v105) misses answered by a retired entry (TextureCache::retire)
  uint64_t hintHits = 0, hintMisses = 0;   // (Claude v102) lookups answered by the producer's hash / hinted but not cached
  uint64_t packHits = 0, packMisses = 0, packReadFail = 0, packVerified = 0, packVerifyBad = 0, packReadTicks = 0, convTicks = 0, packReadBytes = 0, packSwitchedOff = 0;
  uint64_t packAllocTicks = 0, packIoTicks = 0; // Split SD reads from GPU/output allocation; packReadTicks includes both.   // (Claude) offline texture pack (texpack.h)
  size_t entries = 0, bytes = 0;
};

// An image the GPU produced by copying (part of) the EFB: it exists only on the GPU, the guest memory behind its address holds nothing useful.
// The host fills the GPU side (TexEntry::user) right after addRendered(); a binding at the same address / size finds it without touching guest memory.
struct RenderedTexture {
  uint32_t addr = 0;                       // guest address the game will read it from (the copy destination)
  int gxFmt = 0, gxW = 0, gxH = 0;         // what the GX copy produced (copy format, size after the optional half scaling)
  uint32_t guestBytes = 0;                 // size of that image in guest memory (what the copy overwrites there)
  int pfmt = 0;                            // PICA colour format of the allocation (PicaTexFmt numbering, a renderable one)
  int allocW = 0, allocH = 0;              // PICA texture size
  int portW = 0, portH = 0;                // size of the picture inside it (the rest is padding)
  size_t cost = 0;                         // bytes charged against the budget
  std::vector<std::string> notes;          // approximations of this copy (format, resolution)
};

struct TexEntry {
  uint32_t id = 0;
  MappedTexture tex;                       // tex.ok == false: the image cannot be mapped (cached so the failure is not recomputed)
  size_t cost = 0;                         // converted bytes charged against the budget
  uintptr_t user = 0;                      // the host's GPU-side handle for this entry (0 = not uploaded yet)
  // identity
  uint32_t addr = 0;
  size_t srcBytes = 0;                     // guest bytes of the image (all stored mip levels)
  uint64_t sampleHash = 0, fullHash = 0;
  uint64_t lastUseFrame = 0;
  uint64_t sampledFrame = ~uint64_t(0);    // frame whose guest bytes the sample hash was last checked against (sampleOncePerFrame)
  // rendered entries (addRendered): no guest bytes behind them
  bool rendered = false;
  int gxFmt = 0, gxW = 0, gxH = 0, portW = 0, portH = 0;
  std::vector<std::string> renderedNotes;
};

struct TexLookup {
  const TexEntry* entry = nullptr;         // nullptr: no texture (see error)
  PicaSamplerDesc sampler;                 // the sampler of THIS binding
  bool created = false;                    // the entry was converted by this lookup: upload it
  std::string error;
  // How the lookup went, for the draw's log (palette entries that were never loaded, ...)
  std::vector<std::string> notes;
  TexEntry* mutableEntry() const { return const_cast<TexEntry*>(entry); }   // the host stores its GPU handle in TexEntry::user
};

class TextureCache {
 public:
  explicit TextureCache(const TexCacheOptions& o = {}) : opt_(o) {}
  TlutMemory& tlut() { return tlut_; }
  using OutputAllocator=std::function<uint8_t*(TexEntry&,int,int,int,int,const std::vector<size_t>&,size_t)>;
  void setOutputAllocator(OutputAllocator f,std::function<void(TexEntry&)> finish){onOutput_=std::move(f);onOutputReady_=std::move(finish);}
  void setEvictCallback(std::function<void(const TexEntry&)> f) { onEvict_ = std::move(f); }

  TexLookup get(const GxTexBinding& b, const GuestMem& mem, const TexMapOptions& mo = {});
  // (Claude v122) draw replay: true when get() would return e again this frame without reading guest memory: e (not rendered, not paletted) was
  // checked against this frame's bytes by a get() of this frame and no EFB copy has been added since. Never dereferences e.
  bool validThisFrame(const TexEntry* e) const {
    const uint32_t i = quickSlot(e);
    for (uint32_t k = 0; k < kQuickProbe; k++) { const Quick& q = quick_[(i + k) & (kQuickSlots - 1)]; if (q.frame != quickGen_) return false; if (q.e == e) return true; }
    return false;
  }
  // Guest memory in [addr, addr + size) changed: drops converted entries and rendered copies that overlap it.
  void invalidateRange(uint32_t addr, uint32_t size);
  // Drops every converted entry. Rendered copies stay (hook GXInvalidateTexAll here: it flushes the texture cache, not the memory behind it); use invalidateRange for them.
  void invalidateAll();
  // Registers an EFB copy as a texture. A copy overwrites guest memory, so converted entries overlapping its range and older rendered copies there are dropped
  // (the evict callback fires for them). The returned entry has `user == 0`: the host allocates its GPU texture (size allocW x allocH, format pfmt) and sets it.
  TexEntry* addRendered(const RenderedTexture& r);
  // The rendered copy currently living at a guest address (nullptr: none). For hosts that read the copy back (the hwcheck6 dump, debug views).
  const TexEntry* renderedAt(uint32_t addr) const;
  // Copy dimensions may need padding even when the GX binding is power-of-two.
  uint8_t scaledRenderedTexmaps(const GxTexBinding bindings[8]) const;
  // Start of a frame: advances the frame counter, verifies images, evicts. Entries looked up before the next call count as used in this frame.
  void beginFrame(const GuestMem& mem);
  void publishKnown() const;   // (Claude v102) texKnownContains' table
  size_t sampleBytes() const { return opt_.sampleBytes; }

  TexCacheStats stats() const;
  // (Claude) offline texture pack: misses look here before converting. verify: convert anyway and compare (packVerifyBad counts differences).
  void setPack(const TexPackReader* p, bool verify) { pack_ = p; packVerify_ = verify; }
  uint64_t frame() const { return frame_; }
  // (Claude v91) diagnostics (SELECT texture dump): every entry with its key fields
  struct EntryInfo { uint32_t addr; int width, height, fmt, levels, wrapS, wrapT, tlutFmt; bool needAlpha; uint64_t tlutHash; };
  void forEachEntry(const std::function<void(const EntryInfo&, const TexEntry&)>& f) const {
    for (const auto& kv : entries_) { const Key& k = kv.second.first;
      f(EntryInfo{k.addr, k.width, k.height, k.fmt, k.levels, k.wrapS, k.wrapT, k.tlutFmt, k.needAlpha, k.tlutHash}, *kv.second.second); }
  }

 private:
  struct Key {
    uint32_t addr; uint16_t width, height; uint8_t fmt, levels, wrapS, wrapT; bool needAlpha; uint8_t tlutFmt; uint64_t tlutHash;
    bool operator<(const Key& o) const;
  };
  void erase(uint32_t id);   // removes an entry (calls the evict callback)
  // (Claude v105) an entry whose guest bytes changed (verify / sample hash) is kept aside with its GPU copy: games cycle a few images through
  // one buffer (forest: 96x96 IA8 frames), and each return used to cost a pack read from the SD card (4-12 ms). A miss whose full hash and key
  // match a retired entry takes it back. Bounded by kRetiredMax entries / kRetiredBytes; the oldest go through the evict callback.
  void retire(uint32_t id);
  void dropRetired(size_t keep);
  struct Retired { struct Key key; std::unique_ptr<TexEntry> e; };
  std::vector<Retired> retired_;
  size_t retiredBytes_ = 0;
  static constexpr size_t kRetiredMax = 24, kRetiredBytes = 1u << 20;

  struct PaletteMemo {
    bool valid=false; int fmt=0, entries=0, missing=0; uint32_t offset=0;
    uint64_t generation=0, hash=0;
  };
  PaletteMemo paletteMemo_[16];
  TexCacheOptions opt_;
  const TexPackReader* pack_ = nullptr;
  bool packOff_ = false;   // (Claude v133) pack slower than conversion: off for the session
  bool packVerify_ = false;
  bool loadFromPack(const GxTexBinding& b, const TexPackRecord& r, TexEntry& e);
  TlutMemory tlut_;
  OutputAllocator onOutput_;
  std::function<void(TexEntry&)> onOutputReady_;
  std::function<void(const TexEntry&)> onEvict_;
  std::map<Key, uint32_t> byKey_;
  std::map<uint32_t, uint32_t> rendered_;  // guest address -> entry id of the rendered copy living there
  std::map<uint32_t, std::pair<Key, std::unique_ptr<TexEntry>>> entries_;   // by id, so verification can continue where it stopped
  uint32_t nextId_ = 1;
  uint64_t frame_ = 0;
  // (Claude v122) entries get() validated this frame (open addressing, stamped with quickGen_ instead of cleared)
  static constexpr uint32_t kQuickSlots = 2048, kQuickProbe = 8;
  struct Quick { const TexEntry* e = nullptr; uint64_t frame = ~uint64_t(0); };
  Quick quick_[kQuickSlots];
  uint64_t quickGen_ = 1;
  static uint32_t quickSlot(const TexEntry* e) { return uint32_t((uintptr_t(e) >> 3) * 2654435761u) >> 21; }
  void markValid(const TexEntry* e) {
    const uint32_t i = quickSlot(e);
    for (uint32_t k = 0; k < kQuickProbe; k++) { Quick& q = quick_[(i + k) & (kQuickSlots - 1)]; if (q.frame != quickGen_) { q.e = e; q.frame = quickGen_; return; } if (q.e == e) return; }
  }
  TexLookup getImpl(const GxTexBinding& b, const GuestMem& mem, const TexMapOptions& mo);
  uint32_t verifyCursor_ = 0;              // id after which the next verification pass continues
  size_t bytes_ = 0;
  TexCacheStats st_;
};

uint64_t hashBytes(const uint8_t* p, size_t n, uint64_t seed = 0);   // fast 64 bit hash (8 bytes per step)

}  // namespace t2p
