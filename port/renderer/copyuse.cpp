// copyuse (PC, analysis only): for every EFB texture copy, whether a LATER FRAME samples its destination before overwriting it,
// and where in its frame the last copy sits (draws before / after). Conservative: a draw "samples" every texmap its TEV stages reference,
// and every valid texmap when indirect texturing is configured. Usage: copyuse replay.wwrp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include "gxfifo.h"
#include "gxstate.h"
using namespace t2p;
struct Mem1 { std::vector<uint8_t> bytes; const uint8_t* read(uint32_t a, uint32_t n) const { a &= 0x01FFFFFF; return uint64_t(a) + n <= bytes.size() ? bytes.data() + a : nullptr; } };
struct Ev { int kind; uint32_t addr; std::vector<uint32_t> reads; };   // 0 draw, 1 texcopy, 2 xfb
struct Sink : GxSink {
  Mem1* mem = nullptr; std::vector<Ev> evs;
  void onDraw(const GxRegs& r, const GxDraw& d) override {
    GxState S = decodeState(r, &d, false);
    Ev e{0, 0, {}};
    std::set<int> maps;
    for (int i = 0; i < S.numTevStages && i < (int)S.draw.tev.stages.size(); i++) if (S.draw.tev.stages[i].texmap >= 0) maps.insert(S.draw.tev.stages[i].texmap & 7);
    if (S.draw.indirect) for (int m = 0; m < 8; m++) maps.insert(m);
    for (int m : maps) if (S.tex[m].valid) e.reads.push_back(S.tex[m].addr & 0x01FFFFFF);
    evs.push_back(std::move(e));
  }
  void onCopy(const GxRegs& r) override { GxCopy c = decodeCopy(r); evs.push_back(Ev{c.toXfb ? 2 : 1, c.dstAddr & 0x01FFFFFF, {}}); }
  const uint8_t* resolveDisplayList(uint32_t addr, uint32_t size) override { return mem->read(addr, size); }
};
int main(int argc, char** argv) {
  std::vector<uint8_t> f; { FILE* in = std::fopen(argv[1], "rb"); uint8_t b[1 << 16]; size_t n; while ((n = std::fread(b, 1, sizeof b, in)) > 0) f.insert(f.end(), b, b + n); std::fclose(in); }
  uint32_t hdr[3]; std::memcpy(hdr, &f[4], 12);
  Mem1 mem; mem.bytes.assign(hdr[1], 0);
  Sink sink; sink.mem = &mem; GxFifo fifo(&sink);
  fifo.setGuestArrayReader([&mem](uint32_t a, uint32_t n) { return mem.read(a, n); });
  fifo.setAuroraMode(hdr[0] == 2);
  std::vector<std::vector<Ev>> frames;
  size_t at = 16;
  for (uint32_t i = 0; i < hdr[2]; i++) {
    const uint8_t type = f[at]; at += 4; uint32_t a = 0, n = 0;
    if (type == 0) { std::memcpy(&n, &f[at], 4); at += 4; fifo.process(&f[at], n); at += n; }
    else if (type == 1) { std::memcpy(&a, &f[at], 4); std::memcpy(&n, &f[at + 4], 4); at += 8; std::memcpy(&mem.bytes[a], &f[at], n); at += n; }
    else { at += 4; frames.push_back(std::move(sink.evs)); sink.evs.clear(); }
  }
  // per copy: is its address read in a later frame before a copy to the same address in that frame? (look up to 4 frames ahead)
  for (size_t fi = 0; fi + 1 < frames.size(); fi++) {
    const auto& F = frames[fi];
    int draws = 0, lastCopyDraw = -1, nCopy = 0, crossRead = 0; size_t drawsAfterLastCrossCopy = 0; int lastCrossDraw = -1;
    std::vector<int> drawIdxAt(F.size());
    for (size_t k = 0; k < F.size(); k++) { drawIdxAt[k] = draws; if (F[k].kind == 0) draws++; }
    // within-frame: copies whose dest is overwritten later in the same frame before the end are dead for later frames
    for (size_t k = 0; k < F.size(); k++) {
      if (F[k].kind != 1) continue;
      nCopy++; lastCopyDraw = drawIdxAt[k];
      bool overwritten = false;
      for (size_t j = k + 1; j < F.size(); j++) if (F[j].kind == 1 && F[j].addr == F[k].addr) { overwritten = true; break; }
      bool read = false;
      if (!overwritten)
        for (size_t g = fi + 1; g < frames.size() && g <= fi + 4 && !read; g++) {
          bool killed = false;
          for (const Ev& e : frames[g]) {
            if (e.kind == 0) { for (uint32_t r : e.reads) if (r == F[k].addr) { read = true; break; } }
            else if (e.kind == 1 && e.addr == F[k].addr) { killed = true; }
            if (read || killed) break;
          }
          if (killed) break;
        }
      if (read) { crossRead++; lastCrossDraw = drawIdxAt[k]; }
    }
    std::printf("frame %zu draws %d copies %d lastCopyAtDraw %d crossFrameRead %d lastCrossCopyAtDraw %d\n", fi, draws, nCopy, lastCopyDraw, crossRead, lastCrossDraw);
  }
}
