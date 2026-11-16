// (Claude) checkdump: compares a SELECT texture dump (texdump.bin: what the 3DS converted at run time) with ww_texpack.bin (what the PC converted).
// usage: checkdump texdump.bin ww_texpack.bin     For every direct-format texture in the dump: is it in the pack, and is level 0 byte-identical?
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/texcache.h"
#include "../src/texpack.h"

using namespace t2p;

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  FILE* df = std::fopen(argv[1], "rb"); FILE* pf = std::fopen(argv[2], "rb");
  if (!df || !pf) { std::perror("open"); return 1; }
  TexPackReader rd;
  if (!rd.open([pf](uint64_t o, size_t n, void* d) { return std::fseek(pf, long(o), SEEK_SET) == 0 && std::fread(d, 1, n, pf) == n; }, texPackOptionsSig(true, false, false))) { std::fprintf(stderr, "pack open failed\n"); return 1; }
  size_t same = 0, differ = 0, absent = 0, skipped = 0;
  for (;;) {
    char magic[4];
    if (std::fread(magic, 1, 4, df) != 4) break;
    uint32_t h32[10]; uint64_t h64[3]; uint32_t t32[7];
    std::fread(h32, 4, 10, df); std::fread(h64, 8, 3, df); std::fread(t32, 4, 7, df);
    const uint32_t addr = h32[1]; const int w = int(h32[2]), h = int(h32[3]), fmt = int(h32[4]), levels = int(h32[5]), wrapS = int(h32[6]), wrapT = int(h32[7]), tlutFmt = int(h32[8]), needAlpha = int(h32[9]);
    const int rendered = int(t32[0]), pfmt = int(t32[1]); const uint32_t gb = t32[5], pb = t32[6];
    std::vector<uint8_t> guest(gb), pica(pb);
    std::fread(guest.data(), 1, gb, df); std::fread(pica.data(), 1, pb, df);
    if (rendered || fmt == 8 || fmt == 9 || fmt == 10) { skipped++; continue; }
    GxTexBinding b; b.valid = true; b.width = w; b.height = h; b.fmt = fmt; b.wrapS = uint8_t(wrapS); b.wrapT = uint8_t(wrapT); b.tlutFmt = tlutFmt;
    b.mipMode = levels > 1 ? 1 : 0; b.maxLod = float(levels - 1);
    TexPackKey k;
    if (!texPackKey(b, guest.data(), guest.size(), nullptr, 0, needAlpha != 0, k)) { skipped++; continue; }
    TexPackRecord r;
    if (!rd.find(k, b, r)) { absent++; std::printf("absent %08x %dx%d fmt %d levels %d needAlpha %d\n", addr, w, h, fmt, levels, needAlpha); continue; }
    std::vector<uint8_t> d(r.dataBytes);
    rd.readData(r, d.data());
    const size_t n0 = r.plevels > 1 ? r.levelOffset[1] : d.size();
    if (int(r.pfmt) == pfmt && n0 == pica.size() && std::memcmp(d.data(), pica.data(), n0) == 0) same++;
    else { differ++; std::printf("DIFFER %08x %dx%d fmt %d: pack pica fmt %d %zu bytes, 3DS fmt %d %zu bytes\n", addr, w, h, fmt, r.pfmt, n0, pfmt, pica.size()); }
  }
  std::printf("identical %zu, different %zu, not in pack %zu, skipped (copies / palette formats) %zu\n", same, differ, absent, skipped);
  return differ ? 1 : 0;
}
