// (Claude) Offline texture pack: GX images converted to PICA on the PC (texpack/mktexpack), looked up at run time instead of converting.
//
// A pack entry is exactly what TextureCache::get() -> mapTexture() would produce for one binding, so a hit is indistinguishable from a conversion.
// The key is the cache key without the address: the image bytes (all levels the binding uses), format, size, wrap modes (only for axes where they
// change the result: not power-of-two or smaller than 8), level count, needAlpha and, for colour-indexed formats, the palette entries the image
// actually uses (entries beyond the highest used index may be stale TMEM and never change the decoded texels).
//
// File (little-endian): TexPackHeader, the index (count x TexPackIndex, sorted by keyHash), records (TexPackRecord + data) anywhere after it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "gxstate.h"

namespace t2p {

struct TexPackKey {
  uint64_t imageHash = 0;   // hashBytes(image, gxImageBytes(binding), 0)
  uint64_t paletteKey = 0;  // 0 for direct formats
  uint16_t width = 0, height = 0;
  uint8_t fmt = 0, wrapS = 0, wrapT = 0, levels = 0, needAlpha = 0, tlutFmt = 0, pad[2] = {0, 0};
  uint32_t reserved = 0;   // explicit: the key is hashed and compared as raw bytes
  bool operator==(const TexPackKey& o) const;
  uint64_t hash() const;
};
static_assert(sizeof(TexPackKey) == 32, "TexPackKey layout");

struct TexPackHeader {
  char magic[4] = {'W', 'W', 'T', 'P'};
  uint32_t version = 2;
  uint32_t count = 0;
  uint32_t optionsSig = 0;   // conversion options the pack was built with (texPackOptionsSig); a mismatch disables the pack
};
// v2: the index entry alone describes the texture, so a hit is ONE read (the data, straight into GPU memory). The key is identified by its 64-bit
// hash (24k entries: a false match is ~1e-11 per lookup); the full key stays in the file's records for tools. meta: pica fmt (4 bits), levels (4),
// log2 width (4), log2 height (4), exact (1). Data size, level offsets and scales follow from these and the binding (texPackLayout), which
// mktexpack checks against the conversion for every entry.
struct TexPackIndex { uint64_t keyHash; uint32_t dataOffset; uint32_t meta; };
struct TexPackRecord {
  TexPackKey key;
  uint8_t pfmt = 0, plevels = 0, exact = 0, ok = 0;
  uint16_t pw = 0, ph = 0;
  float scaleS = 1, scaleT = 1;
  uint32_t encodedBytes = 0;
  uint32_t dataOffset = 0, dataBytes = 0;   // data may be shared between records
  uint32_t levelOffset[11] = {};
};

// Key of a binding's image. `tlutRaw`: the palette as stored (big-endian 16-bit entries) starting at the binding's palette, `tlutAvail` entries readable.
// Returns false when the key cannot be formed (a used palette index beyond tlutAvail).
bool texPackKey(const GxTexBinding& b, const uint8_t* image, size_t imageBytes, const uint8_t* tlutRaw, size_t tlutAvail, bool needAlpha, TexPackKey& k);
uint32_t texPackOptionsSig(bool allowLossy, bool tryEtc1, bool fastLossyError);
uint32_t texPackMeta(const TexPackRecord& r);   // 0: not expressible (log2 sizes, level count)
// rec <- meta + binding: fmt, levels, size, level offsets, dataBytes, encodedBytes, scales, exact. False: invalid meta.
bool texPackLayout(uint32_t meta, const GxTexBinding& b, TexPackRecord& rec);

// Reader: index in memory, records read on demand through `read(offset, size, dst)` (file or memory).
class TexPackReader {
 public:
  using ReadFn = std::function<bool(uint64_t, size_t, void*)>;
  bool open(ReadFn read, uint32_t optionsSig);   // false: no usable pack (missing, wrong version or options)
  bool ok() const { return !index_.empty(); }
  size_t count() const { return index_.size(); }
  // Finds `k`; on success `rec` describes it (fmt, size, levels, level offsets, data size and offset, scales from `b`, exact; not `key`).
  bool find(const TexPackKey& k, const GxTexBinding& b, TexPackRecord& rec) const;
  bool readData(const TexPackRecord& rec, void* dst) const { return read_(rec.dataOffset, rec.dataBytes, dst); }
  size_t indexBytes() const { return index_.capacity() * sizeof(TexPackIndex); }
 private:
  ReadFn read_;
  std::vector<TexPackIndex> index_;
};

}  // namespace t2p
static_assert(sizeof(t2p::TexPackRecord) == 104 && sizeof(t2p::TexPackIndex) == 16 && sizeof(t2p::TexPackHeader) == 16, "texpack layout (PC writer = 3DS reader)");
