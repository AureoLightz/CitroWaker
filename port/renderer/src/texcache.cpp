#include <atomic>
#include "texcache.h"
#include "texpack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <tuple>

void (*g_r3dsTrace)(const char*, int) = nullptr;
void (*g_texConvLog)(uint32_t addr, int fmt, int w, int h, size_t bytes, bool fromPack, int why, uint64_t ticks) = nullptr;
#include <unordered_map>
static std::unordered_map<uint32_t, int> s_eraseWhy;   // (Claude v105 diag) 1 invalidateRange 2 invalidateAll 3 verify 4 evict   // (Claude v105 diag)   // debugging: progress points inside a frame (set by replay3ds)
namespace t2p {
namespace {

std::string hex32(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08X", unsigned(v)); return b; }

int paletteEntries(int fmt) {
  switch (fmt) {
    case GXF_C4: return 16;
    case GXF_C8: return 256;
    case GXF_C14X2: return 16384;
    default: return 0;
  }
}

// hash of up to `sampleBytes` bytes spread over [p, p + n): eight evenly spaced pieces (the whole image when it is small)
uint64_t sampleHashOfImpl(const uint8_t* p, size_t n, size_t sampleBytes) {
  if (n <= sampleBytes || sampleBytes < 64) return hashBytes(p, n, n);
  const size_t piece = std::max<size_t>(8, sampleBytes / 8);
  uint64_t h = n;
  for (int i = 0; i < 8; i++) h = hashBytes(p + (n - piece) * size_t(i) / 7, piece, h);
  return h;
}

}  // namespace

uint64_t sampleHashOf(const uint8_t* p, size_t n, size_t sampleBytes) { return sampleHashOfImpl(p, n, sampleBytes); }

// (Claude v102) known-texture table
namespace {
struct KnownSlot { uint32_t addr, bytes; uint64_t hash; };
constexpr uint32_t kKnownSlots = 8192;
KnownSlot g_known[2][kKnownSlots];
std::atomic<int> g_knownPub{-1};
inline uint32_t knownIdx(uint32_t addr, uint64_t hash) { return uint32_t((uint64_t(addr) * 0x9E3779B1u) ^ hash ^ (hash >> 32)) & (kKnownSlots - 1); }
}  // namespace
bool texKnownContains(uint32_t addr, uint32_t bytes, uint64_t hash) {
  const int b = g_knownPub.load(std::memory_order_acquire);
  if (b < 0 || !addr) return false;
  const KnownSlot* t = g_known[b];
  for (uint32_t i = knownIdx(addr, hash), n = 0; n < 64; n++, i = (i + 1) & (kKnownSlots - 1)) {
    if (!t[i].addr) return false;
    if (t[i].addr == addr && t[i].bytes == bytes && t[i].hash == hash) return true;
  }
  return false;
}
// (Claude v116) destinations of EFB texture copies the cache holds as GPU textures (rendered_): bindings there never read guest memory (the copy
// wrote VRAM, not RAM), so the producer's snapshot does not copy them (console: a 450 KB screen copy was memcpy'd every painted frame, ~1.5 ms)
namespace {
constexpr uint32_t kRenderedSlots = 256;
uint32_t g_renderedAddr[2][kRenderedSlots];
std::atomic<int> g_renderedPub{-1};
}  // namespace
bool texRenderedAt(uint32_t addr) {
  const int b = g_renderedPub.load(std::memory_order_acquire);
  if (b < 0 || !addr) return false;
  const uint32_t* t = g_renderedAddr[b];
  for (uint32_t i = (addr * 0x9E3779B1u) >> 24, n = 0; n < kRenderedSlots; n++, i = (i + 1) & (kRenderedSlots - 1)) {
    if (!t[i]) return false;
    if (t[i] == addr) return true;
  }
  return false;
}
void TextureCache::publishKnown() const {
  {
    const int rb = g_renderedPub.load(std::memory_order_relaxed) == 0 ? 1 : 0;
    uint32_t* rt = g_renderedAddr[rb];
    std::memset(rt, 0, sizeof g_renderedAddr[0]);
    size_t n = 0;
    for (const auto& kv : rendered_) {
      if (!kv.first || ++n > kRenderedSlots / 2) break;
      for (uint32_t i = (kv.first * 0x9E3779B1u) >> 24, k = 0; k < kRenderedSlots; k++, i = (i + 1) & (kRenderedSlots - 1))
        if (!rt[i]) { rt[i] = kv.first; break; }
    }
    g_renderedPub.store(rb, std::memory_order_release);
  }
  const int b = g_knownPub.load(std::memory_order_relaxed) == 0 ? 1 : 0;
  KnownSlot* t = g_known[b];
  std::memset(t, 0, sizeof g_known[0]);
  size_t used = 0;
  for (const auto& kv : entries_) {
    const TexEntry& e = *kv.second.second;
    if (e.rendered || !e.tex.ok || !e.addr || !e.srcBytes || e.srcBytes > 0xFFFFFFFFu) continue;
    if (++used > kKnownSlots / 2) break;
    for (uint32_t i = knownIdx(e.addr, e.sampleHash), n = 0; n < 64; n++, i = (i + 1) & (kKnownSlots - 1))
      if (!t[i].addr) { t[i] = {e.addr, uint32_t(e.srcBytes), e.sampleHash}; break; }
  }
  g_knownPub.store(b, std::memory_order_release);
}

uint64_t hashBytes(const uint8_t* p, size_t n, uint64_t seed) {
  uint64_t h = seed * 0x9E3779B97F4A7C15ULL + 0xD6E8FEB86659FD93ULL + n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 29;
  }
  uint64_t tail = 0;
  for (size_t k = 0; i + k < n; k++) tail |= uint64_t(p[i + k]) << (8 * k);
  h = (h ^ tail) * 0x9E3779B97F4A7C15ULL;
  h ^= h >> 32;
  return h;
}

// ---------------------------------------------------------------- palette memory
TlutMemory::TlutMemory() : bytes_(size_t(kUnits) * kUnitBytes, 0), lineLoaded_(size_t(kUnits) * kUnitBytes / kLineBytes, 0) {}

size_t TlutMemory::load(const GxTlutLoad& L, const GuestMem& mem) {
  const uint64_t off = uint64_t(L.tmemOffset) * kUnitBytes;
  if (off >= bytes_.size()) return 0;
  const size_t n = std::min<size_t>(size_t(L.entries) * 2, bytes_.size() - size_t(off));   // hardware loads are whole lines (16 entries)
  if (n == 0) return 0;
  const uint8_t* src = mem.read(L.srcAddr, n);
  if (!src) return 0;
  std::memcpy(&bytes_[size_t(off)], src, n);
  for (size_t l = size_t(off) / kLineBytes; l < (size_t(off) + n) / kLineBytes; l++) lineLoaded_[l] = 1;
  loads_++;
  ++generation_;
  return n;
}

uint64_t TlutMemory::hashOf(int fmt, uint32_t offset, int entries, int& missing) const {
  missing = 0;
  const size_t off = size_t(offset) * kUnitBytes;
  const size_t n = size_t(std::max(0, entries));
  const size_t want = n * 2;
  const size_t avail = off >= bytes_.size() ? 0 : std::min(want, bytes_.size() - off);
  for (size_t e = 0; e < n;) {   // count entries that sit in lines nobody loaded (or beyond the area), line by line
    const size_t byte = off + 2 * e;
    if (byte >= bytes_.size()) { missing += int(n - e); break; }
    const size_t line = byte / kLineBytes;
    const size_t inLine = std::min(size_t((line + 1) * kLineBytes - byte), size_t(n - e) * 2) / 2;
    if (!lineLoaded_[line]) missing += int(inLine);
    e += inLine;
  }
  return hashBytes(avail ? &bytes_[off] : nullptr, avail, uint64_t(fmt) + 1) ^ (uint64_t(entries) << 40);
}

GxTlut TlutMemory::resolve(int fmt, uint32_t offset, int entries, uint64_t& hash, int& missing) const {
  hash = hashOf(fmt, offset, entries, missing);
  const size_t off = size_t(offset) * kUnitBytes;
  std::vector<uint8_t> raw(size_t(std::max(0, entries)) * 2, 0);
  if (off < bytes_.size()) std::memcpy(raw.data(), &bytes_[off], std::min(raw.size(), bytes_.size() - off));
  return makeGxTlut(fmt, raw.data(), entries);
}

// ---------------------------------------------------------------- cache
bool TextureCache::Key::operator<(const Key& o) const {
  return std::tie(addr, width, height, fmt, levels, wrapS, wrapT, needAlpha, tlutFmt, tlutHash) <
         std::tie(o.addr, o.width, o.height, o.fmt, o.levels, o.wrapS, o.wrapT, o.needAlpha, o.tlutFmt, o.tlutHash);
}

void TextureCache::retire(uint32_t id) {
  quickGen_++;   // (Claude v122) an entry leaves: validated pointers are forgotten
  auto it = entries_.find(id);
  if (it == entries_.end()) return;
  TexEntry& e = *it->second.second;
  if (e.rendered || !e.tex.ok || e.cost > kRetiredBytes / 4) { erase(id); return; }
  bytes_ -= e.cost;
  byKey_.erase(it->second.first);
  retiredBytes_ += e.cost;
  retired_.push_back(Retired{it->second.first, std::move(it->second.second)});
  entries_.erase(it);
  while (retired_.size() > kRetiredMax || retiredBytes_ > kRetiredBytes) dropRetired(retired_.size() - 1);
}

void TextureCache::dropRetired(size_t keep) {   // drops the oldest until `keep` remain
  while (retired_.size() > keep) {
    TexEntry& e = *retired_.front().e;
    if (onEvict_) onEvict_(e);
    retiredBytes_ -= e.cost;
    retired_.erase(retired_.begin());
  }
}

void TextureCache::erase(uint32_t id) {
  quickGen_++;   // (Claude v122) an entry leaves: validated pointers are forgotten
  auto it = entries_.find(id);
  if (it == entries_.end()) return;
  if (onEvict_) onEvict_(*it->second.second);
  bytes_ -= it->second.second->cost;
  if (it->second.second->rendered) {
    auto r = rendered_.find(it->second.second->addr);
    if (r != rendered_.end() && r->second == id) rendered_.erase(r);
  } else {
    byKey_.erase(it->second.first);
  }
  entries_.erase(it);
}

TexLookup TextureCache::get(const GxTexBinding& b, const GuestMem& mem, const TexMapOptions& mo) {
  TexLookup R = getImpl(b, mem, mo);
  if (R.entry && R.error.empty() && !R.entry->rendered && R.entry->sampledFrame == frame_ && opt_.sampleOncePerFrame && !paletteEntries(b.fmt)) markValid(R.entry);
  return R;
}

TexLookup TextureCache::getImpl(const GxTexBinding& b, const GuestMem& mem, const TexMapOptions& mo) {
  TexLookup R;
  st_.lookups++;
  if (!b.valid) { R.error = "texture unit has no image"; return R; }
  const size_t bytes = gxImageBytes(b);
  if (bytes == 0) { R.error = "unknown GX texture format " + std::to_string(b.fmt); return R; }
  if (b.width < 1 || b.height < 1 || b.width > 1024 || b.height > 1024) { R.error = "texture size " + std::to_string(b.width) + "x" + std::to_string(b.height) + " outside 1..1024"; return R; }
  {   // an EFB copy lives on the GPU only: found by address and size, guest memory is not consulted
    auto rit = rendered_.find(b.addr);
    if (rit != rendered_.end()) {
      TexEntry& e = *entries_[rit->second].second;
      if (e.gxW == b.width && e.gxH == b.height && b.mipLevels() == std::max(1, e.tex.levels)) {   // (Claude v106) synthesized textures carry levels
        e.lastUseFrame = frame_;
        st_.hits++;
        R.entry = &e;
        R.sampler = samplerFor(b, std::max(1, e.tex.levels));
        for (const std::string& n : e.renderedNotes) R.notes.push_back(n);
        if (b.fmt != e.gxFmt) R.notes.push_back("sampled as GX format " + std::to_string(b.fmt) + " but copied as " + std::to_string(e.gxFmt) + ": the copy's channels are used as they are");
        if ((e.portW != e.tex.width || e.portH != e.tex.height) && (R.sampler.wrapS != 0 || R.sampler.wrapT != 0)) {
          R.notes.push_back("repeat / mirror on a copy whose size is not a power of two: clamped (the picture does not tile)");
          R.sampler.wrapS = R.sampler.wrapT = 0;
        }
        return R;
      }
      R.notes.push_back("a copy of " + std::to_string(e.gxW) + "x" + std::to_string(e.gxH) + " lives at this address but the binding is " + std::to_string(b.width) + "x" + std::to_string(b.height) + ": reading guest memory");
    }
  }
  uint64_t hinted = 0;
  const bool hasHint = mem.hashHint(b.addr, bytes, hinted);   // (Claude v102) the producer hashed instead of copying
  const uint8_t* img = nullptr;
  if (!hasHint) {
  img = mem.read(b.addr, bytes);
  if (!img) { R.error = "texture memory " + hex32(b.addr) + " + " + std::to_string(bytes) + " bytes is not mapped"; return R; }
  }

  const int levels = b.mipLevels();
  Key k{};
  k.addr = b.addr; k.width = uint16_t(b.width); k.height = uint16_t(b.height); k.fmt = uint8_t(b.fmt); k.levels = uint8_t(levels);
  k.wrapS = b.wrapS; k.wrapT = b.wrapT; k.needAlpha = mo.needAlpha; k.tlutFmt = 0; k.tlutHash = 0;
  const int palEntries = paletteEntries(b.fmt);
  int missing = 0;
  if (palEntries) {
    k.tlutFmt = uint8_t(b.tlutFmt);
    // TMEM changes only on successful palette loads or assignment. Keep the
    // memo on this single-owner cache, not in the const TlutMemory interface.
    PaletteMemo& memo = paletteMemo_[(b.tlutOffset ^ uint32_t(b.tlutFmt) ^ uint32_t(palEntries >> 4)) & 15];
    const uint64_t gen = tlut_.generation();
    if (opt_.memoPalettes && memo.valid && memo.generation == gen && memo.fmt == b.tlutFmt && memo.offset == b.tlutOffset && memo.entries == palEntries) {
      k.tlutHash = memo.hash; missing = memo.missing;
    } else {
      k.tlutHash = tlut_.hashOf(b.tlutFmt, b.tlutOffset, palEntries, missing);
      if (opt_.memoPalettes) memo = {true,b.tlutFmt,palEntries,missing,b.tlutOffset,gen,k.tlutHash};
    }
    if (missing == palEntries) R.notes.push_back("palette at TMEM offset " + std::to_string(b.tlutOffset) + " was never loaded (all " + std::to_string(palEntries) + " entries read as zero)");
    else if (missing) R.notes.push_back(std::to_string(missing) + " of " + std::to_string(palEntries) + " palette entries at TMEM offset " + std::to_string(b.tlutOffset) + " were never loaded (read as zero)");
  }

  auto it = byKey_.find(k);
  if (opt_.sampleOncePerFrame && it != byKey_.end()) {
    TexEntry& e = *entries_[it->second].second;
    if (e.sampledFrame == frame_) {   // already checked against this frame's bytes
      e.lastUseFrame = frame_;
      st_.hits++;
      R.entry = &e;
      if (e.tex.ok) R.sampler = samplerFor(b, e.tex.levels);
      else R.error = e.tex.error;
      return R;
    }
  }
  if (hasHint && it != byKey_.end()) {   // (Claude v102) hit by the producer's hash; a miss reads the bytes below
    TexEntry& e = *entries_[it->second].second;
    if (e.sampleHash == hinted) {
      e.sampledFrame = frame_;
      e.lastUseFrame = frame_;
      st_.hits++; st_.hintHits++;
      R.entry = &e;
      if (e.tex.ok) R.sampler = samplerFor(b, e.tex.levels);
      else R.error = e.tex.error;
      return R;
    }
  }
  if (hasHint) {
    st_.hintMisses++;
    img = mem.read(b.addr, bytes);
    if (!img) { R.error = "texture memory " + hex32(b.addr) + " + " + std::to_string(bytes) + " bytes is not mapped"; return R; }
  }
  const uint64_t sh = sampleHashOfImpl(img, bytes, opt_.sampleBytes);
  bool staleNow = false;
  const uint64_t logT0 = g_texConvLog ? texProfTicks() : 0;
  if (it != byKey_.end()) {
    TexEntry& e = *entries_[it->second].second;
    if (e.sampleHash == sh) {
      e.sampledFrame = frame_;   // (key = size, so the byte count is the same)
      e.lastUseFrame = frame_;
      st_.hits++;
      R.entry = &e;
      if (e.tex.ok) R.sampler = samplerFor(b, e.tex.levels);
      else R.error = e.tex.error;
      return R;
    }
    st_.staleSample++;
    staleNow = true;
    retire(it->second);
  }

  uint64_t fullHash = 0; bool haveFull = false;
  if (!retired_.empty()) {
    fullHash = hashBytes(img, bytes, 0); haveFull = true;
    for (size_t i = retired_.size(); i-- > 0;) {
      Retired& r = retired_[i];
      if (r.e->fullHash != fullHash || r.e->srcBytes != bytes || (r.key < k || k < r.key)) continue;
      std::unique_ptr<TexEntry> back = std::move(r.e);
      retiredBytes_ -= back->cost;
      retired_.erase(retired_.begin() + i);
      back->sampleHash = sh; back->sampledFrame = frame_; back->lastUseFrame = frame_;
      TexEntry* raw = back.get();
      bytes_ += back->cost;
      byKey_[k] = raw->id;
      entries_[raw->id] = std::make_pair(k, std::move(back));
      st_.hits++; st_.retiredHits++;
      R.entry = raw;
      R.sampler = samplerFor(b, raw->tex.levels);
      return R;
    }
  }
  st_.misses++;
  std::unique_ptr<TexEntry> e(new TexEntry());
  bool fromPack = false;
  if (pack_) {   // (Claude) converted on the PC ahead of time (texpack.h): identical bytes, no conversion
    TexPackKey pk; size_t avail = 0;
    const uint8_t* raw = palEntries ? tlut_.rawAt(b.tlutOffset, avail) : nullptr;
    TexPackRecord r;
    if (texPackKey(b, img, bytes, raw, avail, mo.needAlpha, pk) && pack_->find(pk, b, r)) {
      const uint64_t t0 = texProfTicks();
      fromPack = loadFromPack(b, r, *e);
      st_.packReadTicks += texProfTicks() - t0;
      if (fromPack) st_.packReadBytes += r.dataBytes;
      if (!fromPack) st_.packReadFail++;
    }
    if (fromPack) st_.packHits++; else st_.packMisses++;
  }
  GxTlut pal;
  if (palEntries && (!fromPack || packVerify_)) {
    uint64_t ph = 0;
    pal = tlut_.resolve(b.tlutFmt, b.tlutOffset, palEntries, ph, missing);
  }
  TexMapOptions mo2 = mo;
  mo2.conv.fastLossyError=opt_.fastLossyError;
  if (!opt_.tryEtc1) mo2.conv.tryEtc1 = false;
  if (fromPack && packVerify_) {   // DIAGNOSTIC: the run-time conversion must give the pack's bytes
    const MappedTexture M = mapTexture(b, img, bytes, palEntries ? &pal : nullptr, mo2);
    st_.packVerified++;
    const MappedTexture& P = e->tex;
    if (!M.ok || M.fmt != P.fmt || M.width != P.width || M.height != P.height || M.levels != P.levels || M.data != P.data ||
        M.scaleS != P.scaleS || M.scaleT != P.scaleT || M.exact != P.exact || M.levelOffset != P.levelOffset) st_.packVerifyBad++;
  }
  if (!fromPack) {
    if(onOutput_) mo2.conv.output=[this,entry=e.get()](int f,int w,int h,int levels,const std::vector<size_t>& offsets,size_t size){return onOutput_(*entry,f,w,h,levels,offsets,size);};
    const uint64_t t0 = texProfTicks();
    e->tex = mapTexture(b, img, bytes, palEntries ? &pal : nullptr, mo2);
    st_.convTicks += texProfTicks() - t0;
  }
  if(e->tex.externalOutput && onOutputReady_)onOutputReady_(*e);
  if (g_texConvLog) { int why = staleNow ? 5 : 0; auto w = s_eraseWhy.find(b.addr); if (!staleNow && w != s_eraseWhy.end()) { why = w->second; s_eraseWhy.erase(w); } g_texConvLog(b.addr, b.fmt, b.width, b.height, bytes, fromPack, why * 10 + int(hasHint), texProfTicks() - logT0); }
  st_.conversions++;
  e->id = nextId_++;
  e->cost = e->tex.ok ? e->tex.encodedBytes : 64;
  e->addr = b.addr;
  e->srcBytes = bytes;
  e->sampleHash = sh;
  e->sampledFrame = frame_;
  e->fullHash = haveFull ? fullHash : hashBytes(img, bytes, 0);
  e->lastUseFrame = frame_;
  TexEntry* raw = e.get();
  bytes_ += e->cost;
  byKey_[k] = raw->id;
  entries_[raw->id] = std::make_pair(k, std::move(e));
  R.entry = raw;
  R.created = true;
  if (raw->tex.ok) R.sampler = samplerFor(b, raw->tex.levels);
  else R.error = raw->tex.error;
  return R;
}

// (Claude) a pack record -> e.tex, the data read straight into the GPU texture when the host allocates it (directEncode), else into tex.data
bool TextureCache::loadFromPack(const GxTexBinding& b, const TexPackRecord& r, TexEntry& e) {
  MappedTexture M;
  M.fmt = r.pfmt; M.width = r.pw; M.height = r.ph; M.levels = r.plevels; M.scaleS = r.scaleS; M.scaleT = r.scaleT; M.exact = r.exact != 0;
  M.encodedBytes = r.encodedBytes;
  if (M.levels < 1 || M.levels > 11) return false;
  for (int l = 0; l < M.levels; l++) M.levelOffset.push_back(r.levelOffset[l]);
  // Keep I/O separate: the old "read" timer also includes allocation and eviction.
  const auto readPack = [&](void* dst) {
    const uint64_t t = texProfTicks();
    const bool ok = pack_->readData(r, dst);
    st_.packIoTicks += texProfTicks() - t;
    return ok;
  };
  if (onOutput_ && !packVerify_) {
    const uint64_t t = texProfTicks();
    uint8_t* dst = onOutput_(e, M.fmt, M.width, M.height, M.levels, M.levelOffset, r.dataBytes);
    st_.packAllocTicks += texProfTicks() - t;
    if (dst) {
      if (!readPack(dst)) { if (onEvict_) onEvict_(e); e.user = 0; return false; }
      M.externalOutput = true;
    }
  }
  if (!M.externalOutput) {
    const uint64_t t = texProfTicks();
    M.data.resize(r.dataBytes);
    st_.packAllocTicks += texProfTicks() - t;
    if (!readPack(M.data.data())) return false;
  }
  M.sampler = samplerFor(b, M.levels);
  M.ok = true;
  e.tex = std::move(M);
  return true;
}

void TextureCache::invalidateRange(uint32_t addr, uint32_t size) {
  if (size == 0) return;
  const uint64_t end = uint64_t(addr) + size;
  std::vector<uint32_t> dead;
  for (const auto& kv : entries_) {
    const TexEntry& e = *kv.second.second;
    if (uint64_t(e.addr) < end && uint64_t(e.addr) + e.srcBytes > addr) dead.push_back(e.id);
  }
  for (uint32_t id : dead) { if (g_texConvLog) s_eraseWhy[entries_[id].second->addr] = 1; erase(id); st_.invalidated++; }
}

void TextureCache::invalidateAll() {
  std::vector<uint32_t> dead;
  for (const auto& kv : entries_) if (!kv.second.second->rendered) dead.push_back(kv.first);
  for (uint32_t id : dead) { if (g_texConvLog) s_eraseWhy[entries_[id].second->addr] = 2; erase(id); st_.invalidated++; }
  dropRetired(0);
}

const TexEntry* TextureCache::renderedAt(uint32_t addr) const {
  const auto it = rendered_.find(addr);
  if (it == rendered_.end()) return nullptr;
  const auto e = entries_.find(it->second);
  return e == entries_.end() ? nullptr : e->second.second.get();
}

TexEntry* TextureCache::addRendered(const RenderedTexture& r) {
  const uint32_t guest = r.guestBytes ? r.guestBytes : 1u;
  invalidateRange(r.addr, guest);   // the copy overwrote that memory: converted entries and older copies in the range are stale
  std::unique_ptr<TexEntry> e(new TexEntry());
  e->tex.ok = true;
  e->tex.fmt = r.pfmt; e->tex.width = r.allocW; e->tex.height = r.allocH; e->tex.levels = 1;
  e->tex.scaleS = r.allocW > 0 ? float(r.portW) / float(r.allocW) : 1.0f;
  e->tex.scaleT = r.allocH > 0 ? float(r.portH) / float(r.allocH) : 1.0f;
  e->tex.exact = r.notes.empty();
  e->tex.notes = r.notes;
  e->id = nextId_++;
  e->cost = r.cost;
  e->addr = r.addr;
  e->srcBytes = guest;
  e->lastUseFrame = frame_;
  e->rendered = true;
  e->gxFmt = r.gxFmt; e->gxW = r.gxW; e->gxH = r.gxH; e->portW = r.portW; e->portH = r.portH;
  e->renderedNotes = r.notes;
  TexEntry* raw = e.get();
  bytes_ += e->cost;
  rendered_[r.addr] = raw->id;
  quickGen_++;   // (Claude v122) a copy may now shadow a validated guest texture at its address
  Key k{};   // (not used for lookups: rendered entries are found through rendered_; the entry map is keyed by id)
  k.addr = r.addr;
  entries_[raw->id] = std::make_pair(k, std::move(e));
  return raw;
}

#define TCT(w, i) do { if (g_r3dsTrace) g_r3dsTrace(w, int(i)); } while (0)
void TextureCache::beginFrame(const GuestMem& mem) {
  frame_++;
  quickGen_++;   // (Claude v122) nothing is validated for the new frame yet
  TCT("tc entries", entries_.size()); TCT("tc bytes", bytes_);
  // ---- verification: re-hash whole images round-robin within the byte budget
  if (opt_.verifyBytesPerFrame && !entries_.empty()) {
    size_t budget = opt_.verifyBytesPerFrame;
    size_t visited = 0;
    const size_t total = entries_.size();
    auto it = entries_.upper_bound(verifyCursor_);
    std::vector<uint32_t> dead;
    while (visited < total) {
      if (it == entries_.end()) it = entries_.begin();
      const TexEntry& e = *it->second.second;
      if (e.rendered) { verifyCursor_ = e.id; visited++; ++it; continue; }   // nothing in guest memory to compare with
      if (visited > 0 && e.srcBytes > budget) break;
      budget -= std::min(budget, e.srcBytes);
      TCT("tc verify id", e.id); TCT("tc verify addr", e.addr); TCT("tc verify bytes", e.srcBytes);
      const uint8_t* img = mem.read(e.addr, e.srcBytes);
      if (!img || hashBytes(img, e.srcBytes, 0) != e.fullHash) dead.push_back(e.id);
      verifyCursor_ = e.id;
      visited++;
      ++it;
    }
    TCT("tc dead", dead.size());
    for (uint32_t id : dead) { TCT("tc erase", id); if (g_texConvLog) s_eraseWhy[entries_[id].second->addr] = 3; retire(id); st_.staleVerify++; }
  }
  // ---- eviction: least recently used first, never within protectFrames of the last use
  if (bytes_ > opt_.budgetBytes) {
    std::vector<std::pair<uint64_t, uint32_t>> cand;
    for (const auto& kv : entries_) {
      const TexEntry& e = *kv.second.second;
      if (e.lastUseFrame + uint64_t(opt_.protectFrames) < frame_) cand.push_back({e.lastUseFrame, e.id});
    }
    std::sort(cand.begin(), cand.end());
    for (const auto& c : cand) {
      if (bytes_ <= opt_.budgetBytes) break;
      TCT("tc evict", c.second);
      if (g_texConvLog) s_eraseWhy[entries_[c.second].second->addr] = 4;
      erase(c.second);
      st_.evicted++;
    }
  }
}

uint8_t TextureCache::scaledRenderedTexmaps(const GxTexBinding bindings[8]) const {
  uint8_t mask=0;
  if (rendered_.empty()) return mask;
  for (int m=0;m<8;++m) {
    const auto& b=bindings[m];
    if (!b.valid) continue;
    const auto* e=renderedAt(b.addr);
    // Mirror get(): a differently sized/mipmapped binding reads guest memory.
    if (e && e->gxW==b.width && e->gxH==b.height && b.mipLevels()==std::max(1, e->tex.levels) &&
        (e->tex.scaleS!=1.0f || e->tex.scaleT!=1.0f)) mask |= uint8_t(1u<<m);
  }
  return mask;
}

TexCacheStats TextureCache::stats() const {
  TexCacheStats s = st_;
  s.entries = entries_.size();
  s.bytes = bytes_;
  return s;
}

}  // namespace t2p
