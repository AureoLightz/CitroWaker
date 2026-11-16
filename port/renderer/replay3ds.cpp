// replay3ds: plays a recorded GameCube frame sequence (a Dolphin FIFO log converted by dff2replay.py) through the port's frame loop (src3ds/renderer3ds.h).
//   sdmc:/ww_replay.wwrp  ->  records: GX command bytes (Renderer3ds::process), guest memory writes (textures, vertex arrays, display lists), frame ends (renderFrame)
// Vertex arrays are read from the replayed guest memory through the CP array registers (GxFifo::setGuestArrayReader), display lists through the resolver.
// Output (sdmc:/): replay3ds_result.txt (per frame and per refused draw, then the notes the translator gave, counted), replay3ds_frames.bin (name[32] + 240*400*4 RGBA per frame,
// the same layout as hwcheck6/7 frames). The last frame stays on the top screen until START.
#include <3ds.h>
#include <citro3d.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gxfifo.h"
#include "renderer3ds.h"
#include "texcache.h"
#include "xfbcopy.h"

u32 __stacksize__ = 0x100000;

static const int TW = 240, TH = 400;
static FILE* g_out = nullptr;
static FILE* g_frames = nullptr;
static C3D_RenderTarget* g_tgt = nullptr;
static std::string g_pending;          // name of the frame whose picture the next FrameBegin reads back
static int g_frameNo = -1;
static std::map<std::string, int> g_notes, g_errors;
static int g_drawsThisFrame = 0;

// Guest memory: MEM1 as one flat block at physical address 0.
struct Mem1 : t2p::GuestMem {
  std::vector<uint8_t> bytes;
  const uint8_t* read(uint32_t addr, size_t size) const override {
    addr &= 0x03FFFFFF;
    if (size_t(addr) + size > bytes.size()) return nullptr;
    return &bytes[addr];
  }
};

extern void (*g_r3dsTrace)(const char*, int);
static void r3dsTrace(const char* w, int i) { if (g_frameNo >= 315 && g_out) { std::fprintf(g_out, "X\t%s %d\n", w, i); std::fflush(g_out); } }
static void onFrameBegin() {
  if (g_pending.empty()) return;
  u8* buf = reinterpret_cast<u8*>(g_tgt->frameBuf.colorBuf);
  GSPGPU_InvalidateDataCache(buf, TW * TH * 4);
  char name[32] = {};
  std::snprintf(name, sizeof name, "%s", g_pending.c_str());
  const bool wrote = g_frames && std::fwrite(name, 1, sizeof name, g_frames) == sizeof name && std::fwrite(buf, 1, size_t(TW) * TH * 4, g_frames) == size_t(TW) * TH * 4;
  if (g_frames) std::fflush(g_frames);
  std::fprintf(g_out, "P\t%s\t%s\n", g_pending.c_str(), wrote ? "written" : "WRITE FAILED");
  std::fflush(g_out);
  g_pending.clear();
}

static u64 g_t0 = 0, g_processTicks = 0;
static double g_bms = 0, g_pms = 0, g_dms = 0;
static void onDrawReport(const t2p::DrawReport& D) {
  g_drawsThisFrame++;
  g_bms += D.buildMs; g_pms += D.planMs; g_dms += D.drawMs;
  if (g_drawsThisFrame % 50 == 0) {
    std::fprintf(g_out, "T\tframe %d draw %d\tt=%.1f s\tbuild %.0f plan %.0f draw %.0f ms\n", g_frameNo, g_drawsThisFrame, (osGetTime() - g_t0) / 1000.0, g_bms, g_pms, g_dms);
    std::fflush(g_out);
  }
  if (!D.drawn)
    std::fprintf(g_out, "D\t%d\t%d\tpacket=%d\tplan=%d\tverts=%u\t%s\n", g_frameNo, D.index, int(D.packetOk), int(D.planOk), unsigned(D.vertices), D.why.c_str());
  if (D.packetNotes) for (const std::string& n : *D.packetNotes) g_notes["packet: " + n]++;
  if (D.planNotes) for (const std::string& n : *D.planNotes) g_notes["plan: " + n]++;
  if (!D.drawn) g_notes["NOT DRAWN: " + D.why]++;
}

static void onCopyReport(const t2p::CopyReport& C) {
  if (!C.done) std::fprintf(g_out, "C\t%d\t%d\txfbClear=%d\tNOT DONE\t%s\n", g_frameNo, C.index, int(C.toXfbClear), C.why.c_str());
  for (const std::string& n : C.plan->notes) g_notes["copy: " + n]++;
}

static int bail(const char* msg) {
  std::printf("\nERROR: %s\nPress START to exit.\n", msg);
  if (g_out) { std::fprintf(g_out, "# ERROR %s\n", msg); std::fclose(g_out); g_out = nullptr; }
  while (aptMainLoop()) {
    hidScanInput();
    if (hidKeysDown() & KEY_START) break;
    gfxFlushBuffers(); gfxScreenSwapBuffers(GFX_BOTTOM, false); gspWaitForVBlank();
  }
  C3D_Fini(); gfxExit();
  return 1;
}

static bool readExact(FILE* f, void* p, size_t n) { return n == 0 || std::fread(p, 1, n, f) == n; }

int main() {
  gfxInitDefault();
  C3D_Init(0x100000);
  consoleInit(GFX_BOTTOM, nullptr);
  bool isNew = false;
  APT_CheckNew3DS(&isNew);
  if (isNew) osSetSpeedupEnable(true);
  std::printf("replay3ds: recorded frames through the 3DS renderer\n");

  FILE* in = std::fopen("sdmc:/ww_replay.wwrp", "rb");
  if (!in) return bail("sdmc:/ww_replay.wwrp not found (make it with dff2replay.py)");
  g_out = std::fopen("sdmc:/replay3ds_result.txt", "w");
  if (!g_out) return bail("cannot write sdmc:/replay3ds_result.txt");
  g_frames = std::fopen("sdmc:/replay3ds_frames.bin", "wb");
  if (!g_frames) return bail("cannot write sdmc:/replay3ds_frames.bin");

  char magic[4];
  uint32_t hdr[3];
  if (!readExact(in, magic, 4) || std::memcmp(magic, "WWRP", 4) != 0 || !readExact(in, hdr, sizeof hdr) || (hdr[0] != 1 && hdr[0] != 2)) return bail("ww_replay.wwrp: not a version 1 or 2 replay file");
  const uint32_t mem1Bytes = hdr[1], nRec = hdr[2];
  std::fprintf(g_out, "# replay3ds v1 records=%u mem1=%u new3ds=%d target=%dx%d\n", unsigned(nRec), unsigned(mem1Bytes), int(isNew), TW, TH);

  Mem1 mem;
  mem.bytes.assign(mem1Bytes, 0);

  C3D_RenderTarget* tgt = C3D_RenderTargetCreate(TW, TH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
  if (!tgt) return bail("render target create failed");
  C3D_RenderTargetSetOutput(tgt, GFX_TOP, GFX_LEFT, GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
  g_tgt = tgt;

  t2p::RendererOptions ro;
  ro.frame.efbW = 640; ro.frame.efbH = 480; ro.frame.targetW = 400; ro.frame.targetH = 240; ro.frame.fit = t2p::FitMode::Stretch; ro.frame.widescreen = true; ro.frame.rotate = true;   // widescreen: the full 400x240 top screen
  t2p::Renderer3ds R;
  if (!R.init(ro, &mem, tgt)) return bail("renderer init failed");
  R.setLogs(onDrawReport, onCopyReport, [](const std::string& e) { g_errors[e]++; });
  R.setDisplayListResolver([&mem](uint32_t addr, uint32_t size) { return mem.read(addr, size); });
  R.fifo().setGuestArrayReader([&mem](uint32_t addr, uint32_t size) { return mem.read(addr, size); });
  R.fifo().setAuroraMode(hdr[0] == 2);   // version 2: recorded from Aurora (the native port's GX layer)
  R.setFrameBeginHook(onFrameBegin);
  if (false) g_r3dsTrace = r3dsTrace;   // debugging: progress points inside frames >= 315
  // debugging: sdmc:/replay3ds_skip.txt, one experiment per line, each a list of draw ranges "first last ..." (indices within a frame) that are not drawn;
  // the recording is replayed once per line (an empty line = draw everything) and the frames are named "eN frame F"
  static std::vector<std::vector<std::pair<int, int>>> passes;
  static size_t g_pass = 0;
  static std::vector<std::string> passKeys;
  if (FILE* sf = std::fopen("sdmc:/replay3ds_skip.txt", "r")) {
    char line[512];
    while (std::fgets(line, sizeof line, sf)) {
      std::vector<std::pair<int, int>> v;
      int a = 0, b = 0, used = 0;
      const char* q = line;
      while (std::sscanf(q, "%d %d%n", &a, &b, &used) == 2) { v.push_back({a, b}); q += used; }
      passes.push_back(v);
      passKeys.push_back(q);   // words after the ranges: nofog nodepth nocull flat
    }
    std::fclose(sf);
  }
  if (passes.empty()) passes.push_back({});
  if (passKeys.size() < passes.size()) passKeys.resize(passes.size());
  R.setPlanTweak([](t2p::C3dPlan& L, int idx) {
    const std::string& k = passKeys[g_pass];
    if (k.find("nofog") != std::string::npos) L.fx.fog = false;
    if (k.find("nodepth") != std::string::npos) { L.fx.depthTest = false; L.fx.writeMask &= ~0x10; }
    if (k.find("nocull") != std::string::npos) L.fx.cull = 0;
    if (k.find("flat") != std::string::npos) {   // every draw a solid colour picked from its index: shows which triangles reach the screen
      const uint32_t h = uint32_t(idx) * 2654435761u;
      const uint32_t col = 0xFF000000u | (((h >> 8) & 0xFF) | 0x40) | ((((h >> 16) & 0xFF) | 0x40) << 8) | ((((h >> 24) & 0xFF) | 0x40) << 16);
      t2p::HwStage st0 = {0x0EEE0EEEu, 0u, 0u, col, 0u}, pass = {0x000F000Fu, 0u, 0u, 0u, 0u};
      L.texenv.stage[0] = st0;
      for (int i = 1; i < 6; i++) L.texenv.stage[i] = pass;
      L.fx.fog = false; L.fx.logicOp = false; L.fx.eqRgb = L.fx.eqAlpha = 0; L.fx.srcRgb = L.fx.srcAlpha = 1; L.fx.dstRgb = L.fx.dstAlpha = 0; L.fx.alphaTest = false;
    }
  });
  R.setDrawEdit([](t2p::GxState& S, t2p::GxDraw& d, int) {
    const std::string& k = passKeys[g_pass];
    const bool front = k.find("frontonly") != std::string::npos, cross = k.find("crossonly") != std::string::npos;
    if (!front && !cross) return;
    t2p::VtxSetup setup = S.draw.vtx; setup.numPosMtx = S.draw.numPosMtx;
    std::vector<bool> behind(d.v.count);
    for (uint32_t i = 0; i < d.v.count; i++) {
      t2p::VtxInput vi;
      for (int c = 0; c < 3; c++) vi.pos[c] = d.v.pos[3 * i + c];
      if (d.v.hasPmIdx) vi.mtxIdx = d.v.pmIdx[i] / 3;
      behind[i] = t2p::evalVertex(setup, S.uniforms, vi).clip[3] <= 0;
    }
    std::vector<uint32_t> keep;
    for (size_t t = 0; t + 2 < d.tris.size(); t += 3) {
      const bool b = behind[d.tris[t]] || behind[d.tris[t + 1]] || behind[d.tris[t + 2]];
      if (b == cross) keep.insert(keep.end(), d.tris.begin() + t, d.tris.begin() + t + 3);
    }
    if (keep.empty()) { keep.assign(3, 0); }   // a degenerate triangle: the draw stays valid
    d.tris.swap(keep);
  });
  R.setDrawFilter([](int i) { for (auto& s : passes[g_pass]) if (i >= s.first && i <= s.second) return true; return false; });
  {   // materials translated offline on the PC (mkbundle_replay); without it every material is searched here, about a second each
    std::vector<uint8_t> b;
    if (FILE* bf = std::fopen("sdmc:/ww_bundle.bin", "rb")) {
      uint8_t tmp[1 << 14]; size_t k;
      while ((k = std::fread(tmp, 1, sizeof tmp, bf)) > 0) b.insert(b.end(), tmp, tmp + k);
      std::fclose(bf);
    }
    std::string err;
    const bool ok = !b.empty() && R.loadMaterialBundle(b.data(), b.size(), &err);
    std::fprintf(g_out, "# bundle %s entries=%zu %s\n", ok ? "loaded" : (b.empty() ? "none (sdmc:/ww_bundle.bin)" : "REFUSED"), R.bundleEntries(), err.c_str());
    std::printf("material bundle: %s (%zu)\n", ok ? "loaded" : "none", R.bundleEntries());
  }
  std::fflush(g_out);

  std::vector<uint8_t> buf;
  int framesDrawn = 0;
  const u64 t0 = osGetTime();
  g_t0 = t0;
  const long recStart = std::ftell(in);
  for (g_pass = 0; g_pass < passes.size(); g_pass++) {
  std::fseek(in, recStart, SEEK_SET);
  R.setBatchMaxVertices(passKeys[g_pass].find("nobatch") != std::string::npos ? 0 : t2p::kBatchMaxVertices);
  std::fprintf(g_out, "# experiment %zu:", g_pass);
  for (auto& s : passes[g_pass]) std::fprintf(g_out, " skip %d..%d", s.first, s.second);
  std::fprintf(g_out, " %s", passKeys[g_pass].c_str());
  std::fprintf(g_out, "\n");
  for (uint32_t i = 0; i < nRec; i++) {
    uint8_t rh[4];
    if (!readExact(in, rh, 4)) return bail("replay file truncated (record header)");
    uint32_t a = 0, n = 0;
    if (false && i % 100 == 0) { std::fprintf(g_out, "R\trecord %u/%u\tt=%.1f s\n", unsigned(i), unsigned(nRec), (osGetTime() - t0) / 1000.0); std::fflush(g_out); }
    switch (rh[0]) {
      case 0:   // GX command bytes
        if (!readExact(in, &n, 4)) return bail("replay file truncated (stream length)");
        buf.resize(n);
        if (!readExact(in, buf.data(), n)) return bail("replay file truncated (stream)");
        {
          const u64 p0 = svcGetSystemTick();
          const size_t used = R.process(buf.data(), n);
          g_processTicks += svcGetSystemTick() - p0;
          if (used != n) std::fprintf(g_out, "E\t%d\tcommand stream stopped after %zu of %u bytes (record %u)\n", g_frameNo + 1, used, unsigned(n), unsigned(i));
        }
        break;
      case 1:   // guest memory write
        if (!readExact(in, &a, 4) || !readExact(in, &n, 4)) return bail("replay file truncated (memory header)");
        if (size_t(a) + n > mem.bytes.size()) return bail("memory update outside MEM1");
        if (!readExact(in, &mem.bytes[a], n)) return bail("replay file truncated (memory)");
        R.invalidateTextures(a, n);
        break;
      case 2:   // frame end: draw and save
      case 3: { // frame end: draw, do not save
        uint32_t f = 0;
        if (!readExact(in, &f, 4)) return bail("replay file truncated (frame end)");
        g_frameNo = int(f);
        const bool save = rh[0] == 2;
        const bool forced = R.framesReady() == 0;
        g_drawsThisFrame = 0; g_bms = g_pms = g_dms = 0;
        std::fprintf(g_out, "F\tframe %u end record\tt=%.1f s\n", unsigned(f), (osGetTime() - t0) / 1000.0); std::fflush(g_out);
        const u64 r0 = svcGetSystemTick();
        const t2p::FrameReport F = R.renderFrame(forced);
        const double renderMs = double(svcGetSystemTick() - r0) * 1000.0 / SYSCLOCK_ARM11, processMs = double(g_processTicks) * 1000.0 / SYSCLOCK_ARM11;
        g_processTicks = 0;
        std::fprintf(g_out, "W\tframe %u\tprocess (GX decode + batching) %.1f ms\trenderFrame %.1f ms\ttotal %.1f ms\n", unsigned(f), processMs, renderMs, processMs + renderMs);
        if (save && F.rendered) { g_pending = "e" + std::to_string(g_pass) + " frame " + std::to_string(f); framesDrawn++; }
        std::fprintf(g_out, "S\tframe %u\t%s%s\tdraws=%u drawn=%u refused=%u twopass=%u copies=%u/%u tlut=%u fog=%.1f\tbuild %.1f ms plan %.1f ms draw %.1f ms\n", unsigned(f),
                     F.rendered ? "rendered" : "NOT RENDERED", forced ? " (no XFB copy: forced)" : "", unsigned(F.draws), unsigned(F.drawn), unsigned(F.refused), unsigned(F.twoPass),
                     unsigned(F.copiesDone), unsigned(F.copies), unsigned(F.tlutLoads), double(F.fogRange), F.buildMs, F.planMs, F.drawMs);
        std::fflush(g_out);
        std::printf("frame %u: %u/%u draws  (%.1f s)\n", unsigned(f), unsigned(F.drawn), unsigned(F.draws), (osGetTime() - t0) / 1000.0);
        gfxFlushBuffers(); gfxScreenSwapBuffers(GFX_BOTTOM, false);
        break;
      }
      default: return bail("replay file: unknown record type");
    }
  }
  }   // experiments
  std::fclose(in);

  // one more frame with nothing but a GXCopyDisp, so that the hook reads back the last recorded frame
  {
    const uint8_t black[4] = {0, 0, 0, 255};
    const std::vector<uint8_t> x = hw6::xfbCopyBytes(black, false);
    R.process(x.data(), x.size());
    R.renderFrame();
    if (!g_pending.empty()) R.renderFrame(true);
  }

  std::fprintf(g_out, "# frames saved=%d total %.1f s\n", framesDrawn, (osGetTime() - t0) / 1000.0);
  for (const auto& e : g_errors) std::fprintf(g_out, "# error x%d\t%s\n", e.second, e.first.c_str());
  for (const auto& n : g_notes) std::fprintf(g_out, "# note x%d\t%s\n", n.second, n.first.c_str());
  const t2p::Backend3ds::Stats& bs = R.backend().stats();
  const t2p::TexCacheStats cs = R.cache().stats();
  std::fprintf(g_out, "# backend draws=%u refused=%u noArena=%u noCmdBuf=%u splits=%u shaders=%u textures=%u textureBytes=%llu arenaPeak=%llu\n", unsigned(bs.draws), unsigned(bs.refused),
               unsigned(bs.noArena), unsigned(bs.noCmdBuf), unsigned(bs.splits), unsigned(bs.shadersLoaded), unsigned(bs.texturesUploaded),
               static_cast<unsigned long long>(bs.textureBytes), static_cast<unsigned long long>(bs.arenaPeak));
  std::fprintf(g_out, "# texcache lookups=%llu hits=%llu misses=%llu conversions=%llu entries=%zu bytes=%zu\n", static_cast<unsigned long long>(cs.lookups),
               static_cast<unsigned long long>(cs.hits), static_cast<unsigned long long>(cs.misses), static_cast<unsigned long long>(cs.conversions), cs.entries, cs.bytes);
  std::fprintf(g_out, "# done\n");
  std::fclose(g_out); g_out = nullptr;
  std::fclose(g_frames); g_frames = nullptr;
  std::printf("\nDone: %d frames. sdmc:/replay3ds_result.txt\nSTART = exit.\n", framesDrawn);
  while (aptMainLoop()) {
    hidScanInput();
    if (hidKeysDown() & KEY_START) break;
    gspWaitForVBlank();
  }
  R.shutdown();
  C3D_Fini();
  gfxExit();
  return 0;
}
