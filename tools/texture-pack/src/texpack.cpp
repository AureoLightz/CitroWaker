#include "texpack.h"

#include <algorithm>
#include <cstring>

#include "texcache.h"
#include "texconv.h"
#include "texfit.h"

namespace t2p {

namespace {
bool pow2(int n) { return n > 0 && (n & (n - 1)) == 0; }
}  // namespace

bool TexPackKey::operator==(const TexPackKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
uint64_t TexPackKey::hash() const { return hashBytes(reinterpret_cast<const uint8_t*>(this), sizeof *this, 0x5445585041434bULL); }

uint32_t texPackOptionsSig(bool allowLossy, bool tryEtc1, bool fastLossyError) {
  return 0x7E000000u | (allowLossy ? 1u : 0u) | (tryEtc1 ? 2u : 0u) | (fastLossyError ? 4u : 0u);
}

bool texPackKey(const GxTexBinding& b, const uint8_t* image, size_t imageBytes, const uint8_t* tlutRaw, size_t tlutAvail, bool needAlpha, TexPackKey& k) {
  k = TexPackKey{};
  k.imageHash = hashBytes(image, imageBytes, 0);
  k.width = uint16_t(b.width); k.height = uint16_t(b.height); k.fmt = uint8_t(b.fmt); k.levels = uint8_t(b.mipLevels());
  // planAxis: a power-of-two axis of at least 8 texels is the same for every wrap mode
  k.wrapS = (pow2(b.width) && b.width >= 8) ? 0 : b.wrapS;
  k.wrapT = (pow2(b.height) && b.height >= 8) ? 0 : b.wrapT;
  k.needAlpha = needAlpha ? 1 : 0;
  if (b.fmt == GXF_C4 || b.fmt == GXF_C8 || b.fmt == GXF_C14X2) {
    // highest palette index any level uses (padding texels of partial blocks included: they are decoded too)
    unsigned maxIdx = 0;
    if (b.fmt == GXF_C4) { for (size_t i = 0; i < imageBytes; i++) maxIdx = std::max(maxIdx, unsigned(std::max(image[i] >> 4, image[i] & 15))); }
    else if (b.fmt == GXF_C8) { for (size_t i = 0; i < imageBytes; i++) maxIdx = std::max(maxIdx, unsigned(image[i])); }
    else { for (size_t i = 0; i + 1 < imageBytes; i += 2) maxIdx = std::max(maxIdx, unsigned(((image[i] << 8) | image[i + 1]) & 0x3FFF)); }
    const size_t used = size_t(maxIdx) + 1;
    if (!tlutRaw || used > tlutAvail) return false;
    k.tlutFmt = uint8_t(b.tlutFmt);
    k.paletteKey = hashBytes(tlutRaw, used * 2, uint64_t(b.tlutFmt) + 1) ^ (uint64_t(used) << 40);
  }
  return true;
}

namespace {
int picaBits(int f) { static const int b[14] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8}; return f >= 0 && f < 14 ? b[f] : 0; }
int log2u(int v) { int l = 0; while ((1 << l) < v) l++; return (1 << l) == v ? l : -1; }
}  // namespace

uint32_t texPackMeta(const TexPackRecord& r) {
  const int lw = log2u(r.pw), lh = log2u(r.ph);
  if (lw < 3 || lh < 3 || lw > 10 || lh > 10 || r.plevels < 1 || r.plevels > 11 || r.pfmt > 13) return 0;
  return uint32_t(r.pfmt) | uint32_t(r.plevels) << 4 | uint32_t(lw) << 8 | uint32_t(lh) << 12 | (r.exact ? 1u : 0u) << 16 | 1u << 31;
}

bool texPackLayout(uint32_t meta, const GxTexBinding& b, TexPackRecord& r) {
  if (!(meta >> 31)) return false;
  r.pfmt = uint8_t(meta & 15); r.plevels = uint8_t(meta >> 4 & 15); r.pw = uint16_t(1u << (meta >> 8 & 15)); r.ph = uint16_t(1u << (meta >> 12 & 15));
  r.exact = uint8_t(meta >> 16 & 1); r.ok = 1;
  const int bits = picaBits(r.pfmt);
  if (!bits || r.plevels < 1 || r.plevels > 11) return false;
  uint32_t off = 0;
  for (int l = 0; l < r.plevels; l++) { r.levelOffset[l] = off; off += uint32_t(std::max(8, r.pw >> l)) * uint32_t(std::max(8, r.ph >> l)) * uint32_t(bits) / 8; }
  r.dataBytes = r.encodedBytes = off;
  r.scaleS = axisScale(planAxis(b.width, b.wrapS)); r.scaleT = axisScale(planAxis(b.height, b.wrapT));
  return true;
}

bool TexPackReader::open(ReadFn read, uint32_t optionsSig) {
  index_.clear();
  read_ = std::move(read);
  TexPackHeader h;
  if (!read_ || !read_(0, sizeof h, &h)) return false;
  if (std::memcmp(h.magic, "WWTP", 4) != 0 || h.version != 2 || h.optionsSig != optionsSig || h.count == 0) return false;
  std::vector<TexPackIndex> idx;
  try { idx.resize(h.count); } catch (...) { return false; }
  if (!read_(sizeof h, sizeof(TexPackIndex) * h.count, idx.data())) return false;
  index_ = std::move(idx);
  return true;
}

bool TexPackReader::find(const TexPackKey& k, const GxTexBinding& b, TexPackRecord& rec) const {
  if (index_.empty()) return false;
  const uint64_t kh = k.hash();
  auto it = std::lower_bound(index_.begin(), index_.end(), kh, [](const TexPackIndex& e, uint64_t v) { return e.keyHash < v; });
  if (it == index_.end() || it->keyHash != kh) return false;
  if (!texPackLayout(it->meta, b, rec)) return false;
  rec.dataOffset = it->dataOffset;
  return true;
}

}  // namespace t2p
