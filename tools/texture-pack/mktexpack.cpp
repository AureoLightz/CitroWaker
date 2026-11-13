// (Claude) mktexpack: texsrc.bin (extract_textures.py) -> ww_texpack.bin, the textures converted to PICA ahead of time.
// usage: mktexpack texsrc.bin ww_texpack.bin [--threads N] [--verify]
// Every texture is converted with the run-time options of the 3DS renderer (Renderer3ds texCache: no ETC1, lossy fallback allowed, exact error)
// for each binding variant the game may use: needAlpha 0/1, level count {1, all stored, the BTI's max LOD}, and every wrap mode on axes where it
// matters. --verify re-reads the written pack and compares every record with a fresh conversion.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "src/texcache.h"
#include "src/texconv.h"
#include "src/texmap.h"
#include "src/texpack.h"

using namespace t2p;

namespace {
struct Src {
  int fmt, wrapS, wrapT, mips, w, h, tlutFmt, mipFlag, minLod, maxLod, ncol;
  std::vector<uint8_t> img, pal;
  std::string name;
};

struct Out { TexPackRecord rec; std::vector<uint8_t> data; GxTexBinding b; };

std::atomic<size_t> g_layoutMismatch{0};
bool pow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

std::vector<Src> load(const char* path) {
  std::vector<Src> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) { std::perror(path); std::exit(1); }
  if (std::fseek(f, 0, SEEK_END) != 0) { std::fprintf(stderr,"cannot size texsrc\n"); std::exit(1); }
  const long inputBytes=std::ftell(f);
  if(inputBytes<12 || inputBytes>256L*1024*1024) { std::fprintf(stderr,"texsrc must be 12 bytes to 256 MiB\n"); std::exit(1); }
  std::rewind(f);
  char magic[4]; uint32_t ver, n;
  if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "WWTS", 4) || std::fread(&ver, 4, 1, f) != 1 || std::fread(&n, 4, 1, f) != 1) { std::fprintf(stderr, "bad texsrc\n"); std::exit(1); }
  if(ver!=1 || n==0 || n>uint32_t((inputBytes-12)/28)) { std::fprintf(stderr,"unsupported version or impossible record count\n"); std::exit(1); }
  for (uint32_t i = 0; i < n; i++) {
#pragma pack(push, 1)
    struct { uint8_t fmt, ws, wt, mips; uint16_t w, h; uint8_t tf, mf; int8_t mnl, mxl; uint16_t ncol, pad; uint32_t ni, np, nn; } r;
#pragma pack(pop)
    if (std::fread(&r, sizeof r, 1, f) != 1) { std::fprintf(stderr, "short texsrc\n"); std::exit(1); }
    const long at=std::ftell(f);
    if(r.w<1 || r.h<1 || r.w>1024 || r.h>1024 || r.mips<1 || r.mips>11 || r.np>32768 || r.nn>65535 ||
       at<0 || uint64_t(r.ni)+r.np+r.nn>uint64_t(inputBytes-at)) { std::fprintf(stderr,"invalid or truncated texture record\n"); std::exit(1); }
    Src s{r.fmt, r.ws, r.wt, r.mips, r.w, r.h, r.tf, r.mf, r.mnl, r.mxl, r.ncol, {}, {}, {}};
    s.img.resize(r.ni); s.pal.resize(r.np); s.name.resize(r.nn);
    if (std::fread(s.img.data(), 1, r.ni, f) != r.ni || std::fread(s.pal.data(), 1, r.np, f) != r.np || std::fread(s.name.data(), 1, r.nn, f) != r.nn) { std::fprintf(stderr, "short texsrc\n"); std::exit(1); }
    v.push_back(std::move(s));
  }
  std::fclose(f);
  return v;
}

int paletteEntriesOf(int fmt) { return fmt == GXF_C4 ? 16 : fmt == GXF_C8 ? 256 : fmt == GXF_C14X2 ? 16384 : 0; }

bool g_etc1 = false;   // --etc1: also try ETC1/ETC1A4 (too slow for the console, fine here); same error limits as the run-time lossy fallback
TexMapOptions runtimeOptions(bool needAlpha) {   // Renderer3ds::Options::texCache + TextureCache::get
  TexMapOptions mo;
  mo.needAlpha = needAlpha;
  mo.conv.fastLossyError = false;
  mo.conv.tryEtc1 = g_etc1;
  return mo;
}

bool convert(const Src& s, const GxTexBinding& b, bool needAlpha, Out& o) {
  const size_t bytes = gxImageBytes(b);
  if (bytes == 0 || bytes > s.img.size()) return false;
  std::vector<uint8_t> palRaw;
  GxTlut pal;
  const int pe = paletteEntriesOf(b.fmt);
  if (pe) { palRaw.assign(size_t(pe) * 2, 0); std::memcpy(palRaw.data(), s.pal.data(), std::min(palRaw.size(), s.pal.size())); pal = makeGxTlut(b.tlutFmt, palRaw.data(), pe); }
  if (!texPackKey(b, s.img.data(), bytes, pe ? s.pal.data() : nullptr, s.pal.size() / 2, needAlpha, o.rec.key)) return false;
  MappedTexture M = mapTexture(b, s.img.data(), bytes, pe ? &pal : nullptr, runtimeOptions(needAlpha));
  if (!M.ok || M.levels < 1 || M.levels > 11) return false;
  TexPackRecord& r = o.rec;
  r.ok = 1; r.pfmt = uint8_t(M.fmt); r.plevels = uint8_t(M.levels); r.exact = M.exact ? 1 : 0;
  r.pw = uint16_t(M.width); r.ph = uint16_t(M.height); r.scaleS = M.scaleS; r.scaleT = M.scaleT;
  r.encodedBytes = uint32_t(M.encodedBytes ? M.encodedBytes : M.data.size());
  for (int l = 0; l < M.levels; l++) r.levelOffset[l] = uint32_t(l < int(M.levelOffset.size()) ? M.levelOffset[l] : 0);
  o.data = std::move(M.data);
  r.dataBytes = uint32_t(o.data.size());
  o.b = b;
  // v2 index: the layout must follow from the meta + binding exactly, else the texture stays out of the pack (converted at run time)
  TexPackRecord L;
  const uint32_t meta = texPackMeta(r);
  if (!meta || !texPackLayout(meta, b, L) || L.pfmt != r.pfmt || L.plevels != r.plevels || L.pw != r.pw || L.ph != r.ph || L.exact != r.exact ||
      L.dataBytes != r.dataBytes || L.encodedBytes != r.encodedBytes || L.scaleS != r.scaleS || L.scaleT != r.scaleT ||
      std::memcmp(L.levelOffset, r.levelOffset, sizeof r.levelOffset) != 0) { g_layoutMismatch++; return false; }
  return true;
}

std::vector<GxTexBinding> variants(const Src& s) {
  std::set<int> lv{1, s.mips};
  int top = 0; for (int m = std::max(s.w, s.h); m > 1; m >>= 1) top++;
  if (s.mipFlag) lv.insert(std::min({top, int(std::floor(s.maxLod / 8.0)), s.mips - 1}) + 1);
  std::vector<int> wsS = (pow2(s.w) && s.w >= 8) ? std::vector<int>{0} : std::vector<int>{0, 1, 2};
  std::vector<int> wsT = (pow2(s.h) && s.h >= 8) ? std::vector<int>{0} : std::vector<int>{0, 1, 2};
  std::vector<GxTexBinding> out;
  for (int L : lv) for (int ws : wsS) for (int wt : wsT) {
    if (L < 1 || L > s.mips) continue;
    GxTexBinding b;
    b.valid = true; b.width = s.w; b.height = s.h; b.fmt = s.fmt; b.wrapS = uint8_t(ws); b.wrapT = uint8_t(wt);
    b.mipMode = L > 1 ? 1 : 0; b.maxLod = float(L - 1); b.tlutFmt = s.tlutFmt;
    if (b.mipLevels() != L) continue;
    out.push_back(b);
  }
  return out;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: mktexpack texsrc.bin ww_texpack.bin [--threads N] [--verify]\n"); return 2; }
  if(FILE* existing=std::fopen(argv[2],"rb")) { std::fclose(existing); std::fprintf(stderr,"Choose a NEW output path; existing files are never replaced.\n"); return 2; }
  int threads = 2; bool verify = false;
  for (int i = 3; i < argc; i++) { if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::atoi(argv[++i]); else if (!std::strcmp(argv[i], "--verify")) verify = true; else if (!std::strcmp(argv[i], "--etc1")) g_etc1 = true; }
  threads = std::max(1, std::min(4, threads));
  const std::vector<Src> src = load(argv[1]);
  std::fprintf(stderr, "%zu source textures, %d threads\n", src.size(), threads);

  std::vector<std::vector<Out>> results(src.size());
  std::atomic<size_t> next{0}, done{0}, failed{0};
  auto work = [&] {
    for (size_t i; (i = next++) < src.size();) {
      for (const GxTexBinding& b : variants(src[i])) for (int na = 0; na < 2; na++) {
        Out o;
        if (convert(src[i], b, na != 0, o)) results[i].push_back(std::move(o)); else failed++;
      }
      const size_t d = ++done;
      if (d % 500 == 0) std::fprintf(stderr, "  %zu / %zu\n", d, src.size());
    }
  };
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; t++) pool.emplace_back(work);
  for (auto& t : pool) t.join();

  // records: unique keys (identical images from different files give the same key), shared data blobs
  std::map<uint64_t, std::vector<const Out*>> byHash;
  std::vector<const Out*> recs;
  size_t dupKeys = 0;
  for (auto& v : results) for (auto& o : v) {
    auto& bucket = byHash[o.rec.key.hash()];
    bool dup = false;
    for (const Out* p : bucket) if (p->rec.key == o.rec.key) { dup = true; if (p->data != o.data) { std::fprintf(stderr, "same key, different data (conversion not deterministic?)\n"); return 1; } }
    if (dup) { dupKeys++; continue; }
    bucket.push_back(&o); recs.push_back(&o);
  }
  std::sort(recs.begin(), recs.end(), [](const Out* a, const Out* b) { return a->rec.key.hash() < b->rec.key.hash(); });
  for (size_t i = 1; i < recs.size(); i++) if (recs[i]->rec.key.hash() == recs[i - 1]->rec.key.hash()) { std::fprintf(stderr, "64-bit key hash collision: the index cannot tell two keys apart\n"); return 1; }

  FILE* f = std::fopen(argv[2], "wbx");
  if (!f) { std::perror(argv[2]); return 1; }
  TexPackHeader h; h.count = uint32_t(recs.size()); h.optionsSig = texPackOptionsSig(true, g_etc1, false);
  std::vector<TexPackIndex> index(recs.size());
  const uint64_t recBase = sizeof h + sizeof(TexPackIndex) * recs.size();
  uint64_t pos = recBase + sizeof(TexPackRecord) * recs.size();
  std::map<uint64_t, std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>>> blobs;
  std::vector<TexPackRecord> recOut(recs.size());
  std::vector<const std::vector<uint8_t>*> dataOrder;
  size_t sharedBlobs = 0;
  for (size_t i = 0; i < recs.size(); i++) {
    TexPackRecord r = recs[i]->rec;
    const std::vector<uint8_t>& d = recs[i]->data;
    const uint64_t dh = hashBytes(d.data(), d.size(), 7);
    uint32_t off = 0; bool found = false;
    for (auto& [p, o] : blobs[dh]) if (*p == d) { off = o; found = true; break; }
    if (!found) {
      pos = (pos + 127) & ~uint64_t(127);   // 128-byte aligned: the 3DS reads straight into GPU texture memory
      if (pos + d.size() > 0xFFFFFFFFull) { std::fprintf(stderr, "pack over 4 GB\n"); return 1; }
      off = uint32_t(pos); blobs[dh].push_back({&d, off}); dataOrder.push_back(&d); pos += d.size();
    } else sharedBlobs++;
    r.dataOffset = off;
    recOut[i] = r;
    index[i] = TexPackIndex{r.key.hash(), off, texPackMeta(r)};
  }
  std::fwrite(&h, sizeof h, 1, f);
  std::fwrite(index.data(), sizeof(TexPackIndex), index.size(), f);
  std::fwrite(recOut.data(), sizeof(TexPackRecord), recOut.size(), f);
  uint64_t at = recBase + sizeof(TexPackRecord) * recs.size();
  static const uint8_t zeros[128] = {};
  for (const auto* d : dataOrder) {
    const uint64_t al = (at + 127) & ~uint64_t(127);
    std::fwrite(zeros, 1, size_t(al - at), f); at = al;
    std::fwrite(d->data(), 1, d->size(), f); at += d->size();
  }
  std::fclose(f);
  size_t lossy = 0, inexact = 0, etc = 0; for (const Out* o : recs) { if (!o->rec.exact) inexact++; if (o->rec.pfmt == PTF_RGB565 || o->rec.pfmt == PTF_RGBA4) lossy++; if (o->rec.pfmt >= PTF_ETC1) etc++; }
  std::fprintf(stderr, "formats: %zu RGB565/RGBA4, %zu ETC1/ETC1A4\n", lossy, etc);
  std::fprintf(stderr, "%zu records (%zu duplicate keys merged, %zu conversions failed or not expressible: %zu layout), %zu data blobs (%zu shared), %.1f MB, index %.0f KB; %zu inexact\n",
               recs.size(), dupKeys, size_t(failed), size_t(g_layoutMismatch), dataOrder.size(), sharedBlobs, double(at) / 1e6, double(index.size() * sizeof(TexPackIndex)) / 1024, inexact);

  if (verify) {   // read back through the run-time reader and compare with the conversions
    FILE* rf = std::fopen(argv[2], "rb");
    TexPackReader rd;
    if (!rd.open([rf](uint64_t o, size_t n, void* d) { return std::fseek(rf, long(o), SEEK_SET) == 0 && std::fread(d, 1, n, rf) == n; }, texPackOptionsSig(true, g_etc1, false))) { std::fprintf(stderr, "verify: open failed\n"); return 1; }
    size_t bad = 0;
    for (const Out* o : recs) {
      TexPackRecord r; std::vector<uint8_t> d;
      if (!rd.find(o->rec.key, o->b, r)) { bad++; continue; }
      d.resize(r.dataBytes);
      if (!rd.readData(r, d.data()) || d != o->data || r.pfmt != o->rec.pfmt || r.pw != o->rec.pw) bad++;
    }
    std::fclose(rf);
    std::fprintf(stderr, "verify: %zu records, %zu bad\n", recs.size(), bad);
    if (bad) return 1;
  }
  return 0;
}
