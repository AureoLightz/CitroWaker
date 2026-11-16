// mkbundle_replay (PC): runs a replay file (dff2replay.py) through the same GX decoding and draw compilation the 3DS frame loop uses, with the material bundle recorder on,
// and writes every translation it had to search for as a bundle the 3DS loads before the first frame (Renderer3ds::loadMaterialBundle). Also prints what the 3DS would see:
// draws per frame, refused packets and their reasons, command stream errors.
//   usage: mkbundle_replay replay.wwrp out_bundle.bin [--effort N]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "bundle.h"
#include "draw.h"
#include "gxbatch.h"
#include "gxfifo.h"
#include "gxstate.h"
#include "submit.h"
#include "texcache.h"

using namespace t2p;

struct Mem1 : GuestMem {
  std::vector<uint8_t> bytes;
  const uint8_t* read(uint32_t addr, size_t size) const override {
    addr &= 0x03FFFFFF;
    if (size_t(addr) + size > bytes.size()) return nullptr;
    return &bytes[addr];
  }
};

struct Sink : GxSink {
  Mem1* mem = nullptr;
  const GxFifo* fifo = nullptr;
  uint32_t batchMax = kBatchMaxVertices;   // same merging as Renderer3ds::onDraw (0: off)
  struct D { GxState st; GxDraw d; };
  std::vector<D> draws;
  size_t gxDraws = 0;
  bool lastIsDraw = false;
  uint64_t lastGen = ~0ull;
  int xfbs = 0;
  std::map<std::string, int> errors;
  void onDraw(const GxRegs& r, const GxDraw& d) override {
    gxDraws++;
    const uint64_t gen = fifo->stateGeneration();
    if (batchMax && lastIsDraw && gen == lastGen && canAppend(draws.back().d, d, batchMax)) {
      D& x = draws.back();
      const int before = maxPosMtx(x.d);
      appendDraw(x.d, d);
      if (maxPosMtx(d) > before) x.st = decodeState(r, &x.d);
      return;
    }
    draws.push_back({decodeState(r, &d), d});
    lastIsDraw = true; lastGen = gen;
  }
  void onCopy(const GxRegs& r) override { lastIsDraw = false; if (decodeCopy(r).toXfb) xfbs++; }
  void onTlutLoad(const GxRegs&) override { lastIsDraw = false; }
  void onError(const std::string& e) override { errors[e]++; }
  const uint8_t* resolveDisplayList(uint32_t addr, uint32_t size) override { return mem->read(addr, size); }
};

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s replay.wwrp out_bundle.bin [--effort N]\n", argv[0]); return 2; }
  int effort = Options().approxEffort;
  for (int i = 3; i + 1 < argc; i++) if (!std::strcmp(argv[i], "--effort")) effort = std::atoi(argv[i + 1]);
  FILE* in = std::fopen(argv[1], "rb");
  if (!in) { std::perror(argv[1]); return 1; }
  std::vector<uint8_t> f;
  { uint8_t b[1 << 16]; size_t n; while ((n = std::fread(b, 1, sizeof b, in)) > 0) f.insert(f.end(), b, b + n); std::fclose(in); }
  if (f.size() < 16 || std::memcmp(f.data(), "WWRP", 4)) { std::fprintf(stderr, "not a replay file\n"); return 1; }
  uint32_t hdr[3]; std::memcpy(hdr, &f[4], 12);
  Mem1 mem; mem.bytes.assign(hdr[1], 0);
  Sink sink; sink.mem = &mem;
  GxFifo fifo(&sink);
  sink.fifo = &fifo;
  for (int i = 3; i < argc; i++) if (!std::strcmp(argv[i], "--no-batch")) sink.batchMax = 0;
  fifo.setGuestArrayReader([&mem](uint32_t a, uint32_t n) { return mem.read(a, n); });
  fifo.setAuroraMode(hdr[0] == 2);   // version 2: a stream from Aurora (the native port), pc_wwrp_capture.cpp

  MaterialBundle rec;
  setBundleRecorder(&rec);
  MaterialBundle seen;          // what was already searched in this run: looked up first, so each material is searched once
  setBundleLookup(&seen);
  Options base;                 // offline: full verification and the full approximation search; frameFogRange per frame
  base.approxEffort = effort;
  FrameConfig fc;
  fc.efbW = 640; fc.efbH = 480; fc.targetW = 400; fc.targetH = 240; fc.fit = FitMode::Pillarbox43; fc.rotate = true; fc.resolve();
  std::map<std::string, int> refused;
  size_t vhist[5] = {}; std::map<int, size_t> prims; uint64_t verts = 0;
  size_t at = 16, totalDraws = 0, totalOk = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < hdr[2]; i++) {
    const uint8_t type = f[at]; at += 4;
    uint32_t a = 0, n = 0;
    if (type == 0) {
      std::memcpy(&n, &f[at], 4); at += 4;
      const size_t used = fifo.process(&f[at], n);
      if (used != n) std::printf("record %u: command stream stopped after %zu of %u bytes\n", i, used, n);
      at += n;
    } else if (type == 1) {
      std::memcpy(&a, &f[at], 4); std::memcpy(&n, &f[at + 4], 4); at += 8;
      std::memcpy(&mem.bytes[a], &f[at], n); at += n;
    } else {
      uint32_t fr; std::memcpy(&fr, &f[at], 4); at += 4;
      std::vector<GxDrawState> states;
      for (auto& d : sink.draws) states.push_back(d.st.draw);
      Options opt = base;
      opt.frameFogRange = frameFogRange(states.data(), states.size());
      int ok = 0;
      for (size_t k = 0; k < sink.draws.size(); k++) {
        const DrawPacket pk = buildDraw(sink.draws[k].st, sink.draws[k].d, opt, fc, nullptr);
        { const GxDraw& g = sink.draws[k].d; vhist[g.vertexCount < 4 ? 0 : g.vertexCount < 16 ? 1 : g.vertexCount < 64 ? 2 : g.vertexCount < 256 ? 3 : 4]++; prims[g.prim]++; verts += g.vertexCount; }
        if (pk.ok) ok++;
        else refused[pk.notes.empty() ? std::string("(no note)") : pk.notes.front()]++;
        if ((k + 1) % 50 == 0) { std::printf("  frame %u: %zu/%zu draws compiled, %zu materials searched\r", fr, k + 1, sink.draws.size(), rec.translations()); std::fflush(stdout); }
      }
      const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::printf("frame %u: %zu GX draws merged into %zu draws\n", fr, sink.gxDraws, sink.draws.size());
      sink.gxDraws = 0;
      std::printf("frame %u: %zu draws, %d compiled, %d refused, xfb copies %d, fog range %.1f   (%zu materials searched so far, %.1f s)\n", fr, sink.draws.size(), ok,
                  int(sink.draws.size()) - ok, sink.xfbs, double(opt.frameFogRange), rec.translations(), s);
      std::printf("  vertices per draw: <4 %zu, 4-15 %zu, 16-63 %zu, 64-255 %zu, 256+ %zu; total vertices %llu; prims:", vhist[0], vhist[1], vhist[2], vhist[3], vhist[4], (unsigned long long)verts);
      for (auto& pr : prims) std::printf(" %02x=%zu", pr.first, pr.second);
      std::printf("\n");
      for (auto& h : vhist) h = 0; prims.clear(); verts = 0;
      // move this frame's searches into the lookup
      { std::vector<uint8_t> tmp = rec.serialize(sourceFingerprint()); MaterialBundle fresh; fresh.load(tmp.data(), tmp.size(), sourceFingerprint()); seen = fresh; setBundleLookup(&seen); }
      totalDraws += sink.draws.size(); totalOk += size_t(ok);
      sink.draws.clear(); sink.xfbs = 0; sink.lastIsDraw = false;
    }
  }
  setBundleRecorder(nullptr);
  setBundleLookup(nullptr);
  for (auto& e : sink.errors) std::printf("stream error x%d: %s\n", e.second, e.first.c_str());
  for (auto& r : refused) std::printf("refused x%d: %s\n", r.second, r.first.c_str());
  const std::vector<uint8_t> out = rec.serialize(sourceFingerprint());
  FILE* o = std::fopen(argv[2], "wb");
  if (!o || std::fwrite(out.data(), 1, out.size(), o) != out.size()) { std::perror(argv[2]); return 1; }
  std::fclose(o);
  std::printf("bundle: %zu translations, %zu approximations, %zu bytes -> %s   (draws %zu, compiled %zu)\n", rec.translations(), rec.approximations(), out.size(), argv[2], totalDraws, totalOk);
  return 0;
}
