// hwcheck7: the same scenes as hwcheck6, but driven through the port's frame loop (src3ds/renderer3ds.h) instead of the hand-written runner:
//   GX bytes -> Renderer3ds::process (GxFifo, events recorded) -> a GXCopyDisp that clears the EFB for the next scene -> Renderer3ds::renderFrame
// The scene streams are the ones of tools/hw6/gen_hw6.c (hwcheck6_data.h); this file appends the GXCopyDisp (BP writes: copy source / size, clear colour and depth, copy command with the
// XFB and clear bits) to each scene. The clear colour of that copy is the NEXT scene's background, so the picture of every scene is drawn onto what the previous frame's clear left
// (instead of the harness clearing the target), which tests the XFB path, the delayed clear and the frame loop. Pictures must equal those of hwcheck6 (compare6.py, same references).
// Output (sdmc:/): hwcheck7_result.txt (same line formats as hwcheck6), hwcheck7_frames.bin (name[32] + 240*400*4 colour buffer per scene), hwcheck7_copies.bin.
#include <3ds.h>
#include <citro3d.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gxfifo.h"
#include "hw6_mem.h"
#include "renderer3ds.h"
#include "xfbcopy.h"

#include "hwcheck6_data.h"

u32 __stacksize__ = 0x100000;

static const int TW = 240, TH = 400;      // rotated top screen target
static FILE* g_out = nullptr;
static FILE* g_frames = nullptr;
static FILE* g_copies = nullptr;

struct Pending { bool have = false; std::string name; };
static Pending g_pending;
struct Dump { std::string name; uint32_t addr; int allocW, allocH, portW, portH, pfmt, gxFmt; };
static std::vector<Dump> g_dumps;
static t2p::Renderer3ds* g_r = nullptr;
static C3D_RenderTarget* g_tgt = nullptr;
static const char* g_scene = "";
static int g_copyIdx = 0;

static void writeDumps() {
  for (const Dump& d : g_dumps) {
    const t2p::TexEntry* e = g_r->cache().renderedAt(d.addr);
    const void* data = nullptr;
    uint32_t bytes = 0;
    const bool have = e && g_r->backend().textureImage(*e, &data, &bytes);
    if (!have) { std::fprintf(g_out, "C\t%s\tdump FAILED (texture gone)\n", d.name.c_str()); bytes = 0; }
    char name[32] = {};
    std::snprintf(name, sizeof name, "%s", d.name.c_str());
    const uint16_t hdr[4] = {uint16_t(d.allocW), uint16_t(d.allocH), uint16_t(d.portW), uint16_t(d.portH)};
    const uint32_t tail[3] = {uint32_t(d.pfmt), uint32_t(d.gxFmt), bytes};
    if (g_copies) {
      std::fwrite(name, 1, sizeof name, g_copies); std::fwrite(hdr, 2, 4, g_copies); std::fwrite(tail, 4, 3, g_copies);
      if (bytes) std::fwrite(data, 1, bytes, g_copies);
      std::fflush(g_copies);
    }
  }
  g_dumps.clear();
}

// inside renderFrame, right after C3D_FrameBegin: the previous scene's frame is complete
static void onFrameBegin() {
  if (!g_pending.have) return;
  u8* buf = reinterpret_cast<u8*>(g_tgt->frameBuf.colorBuf);
  GSPGPU_InvalidateDataCache(buf, TW * TH * 4);
  char name[32] = {};
  std::snprintf(name, sizeof name, "%s", g_pending.name.c_str());
  const bool wrote = g_frames && std::fwrite(name, 1, sizeof name, g_frames) == sizeof name && std::fwrite(buf, 1, size_t(TW) * TH * 4, g_frames) == size_t(TW) * TH * 4;
  if (g_frames) std::fflush(g_frames);
  writeDumps();
  std::fprintf(g_out, "P\t%s\t%s\n", g_pending.name.c_str(), wrote ? "written" : "WRITE FAILED");
  std::fflush(g_out);
  g_pending.have = false;
}

static void onDrawReport(const t2p::DrawReport& D) {
  std::fprintf(g_out, "D\t%s\t%d\tpacket=%d\tplan=%d\tdrawn=%d\tverts=%u\ttris=%u\t%s\n", g_scene, D.index, int(D.packetOk), int(D.planOk), int(D.drawn), unsigned(D.vertices),
               unsigned(D.triangles), D.why.c_str());
  if (D.packetNotes) for (const std::string& n : *D.packetNotes) std::fprintf(g_out, "N\t%s\t%d\tpacket\t%s\n", g_scene, D.index, n.c_str());
  if (D.planNotes) for (const std::string& n : *D.planNotes) std::fprintf(g_out, "N\t%s\t%d\tplan\t%s\n", g_scene, D.index, n.c_str());
  std::fflush(g_out);
}

static void onCopyReport(const t2p::CopyReport& C) {
  const t2p::EfbCopyPlan& P = *C.plan;
  const t2p::GxCopy& c = *C.copy;
  if (C.toXfbClear) {
    std::fprintf(g_out, "X\t%s\tclear of the previous GXCopyDisp\tok=%d\tclears=%d\tdone=%d\t%s\n", g_scene, int(P.ok), int(P.clears), int(C.done), C.why.c_str());
    for (const std::string& n : P.notes) std::fprintf(g_out, "N\t%s\txfb\tplan\t%s\n", g_scene, n.c_str());
    std::fflush(g_out);
    return;
  }
  const t2p::EfbCopyFormat F = t2p::efbCopyFormat(c);
  std::fprintf(g_out, "C\t%s\t%d\tfmt=%s\tok=%d\ttoTexture=%d\tclears=%d\tdone=%d\tsrc=%d,%d,%d,%d\tdst=%08x\tgx=%dx%d\talloc=%dx%d\tport=%dx%d\tpfmt=%d\tplan %.2f ms draw %.2f ms\t%s\n",
               g_scene, g_copyIdx, F.name, int(P.ok), int(P.toTexture), int(P.clears), int(C.done), c.srcX, c.srcY, c.srcW, c.srcH, unsigned(c.dstAddr), P.tex.gxW, P.tex.gxH,
               P.tex.allocW, P.tex.allocH, P.tex.portW, P.tex.portH, P.tex.pfmt, C.planMs, C.drawMs, C.why.c_str());
  for (const std::string& n : P.notes) std::fprintf(g_out, "N\t%s\tcopy%d\tplan\t%s\n", g_scene, g_copyIdx, n.c_str());
  if (C.done && P.toTexture) {
    char nm[32];
    std::snprintf(nm, sizeof nm, "%s#%d", g_scene, g_copyIdx);
    g_dumps.push_back({nm, P.tex.addr, P.tex.allocW, P.tex.allocH, P.tex.portW, P.tex.portH, P.tex.pfmt, P.tex.gxFmt});
  }
  g_copyIdx++;
  std::fflush(g_out);
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

int main() {
  gfxInitDefault();
  C3D_Init(0x100000);
  consoleInit(GFX_BOTTOM, nullptr);
  std::printf("hwcheck7: %d scenes through the frame loop\n", kHw6NumScenes - 1);
  g_out = std::fopen("sdmc:/hwcheck7_result.txt", "w");
  if (!g_out) return bail("cannot write sdmc:/hwcheck7_result.txt");
  std::fprintf(g_out, "# hwcheck7 v1 scenes=%d target=%dx%d\n", kHw6NumScenes - 1, TW, TH);
  g_frames = std::fopen("sdmc:/hwcheck7_frames.bin", "wb");
  if (!g_frames) return bail("cannot write sdmc:/hwcheck7_frames.bin");
  g_copies = std::fopen("sdmc:/hwcheck7_copies.bin", "wb");
  if (!g_copies) return bail("cannot write sdmc:/hwcheck7_copies.bin");

  C3D_RenderTarget* tgt = C3D_RenderTargetCreate(TW, TH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
  if (!tgt) return bail("render target create failed");
  C3D_RenderTargetSetOutput(tgt, GFX_TOP, GFX_LEFT, GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
  g_tgt = tgt;

  hw6::FlatMem mem;
  if (!mem.load(kHw6Mem, kHw6Mem_size)) return bail("guest memory image is corrupt");

  t2p::RendererOptions ro;
  ro.frame.efbW = 640; ro.frame.efbH = 480; ro.frame.targetW = 400; ro.frame.targetH = 240; ro.frame.fit = t2p::FitMode::Pillarbox43; ro.frame.rotate = true;
  for (int i = 0; i < 4; i++) ro.initialClear[i] = kHw6NumScenes > 1 ? kHw6Scenes[1].clear[i] : 0;
  t2p::Renderer3ds R;
  g_r = &R;
  if (!R.init(ro, &mem, tgt)) return bail("renderer init failed");
  R.setLogs(onDrawReport, onCopyReport, [](const std::string& e) { if (g_out) std::fprintf(g_out, "E\t%s\t%s\n", g_scene, e.c_str()); });
  {   // the host's translator results for these scenes (hw6_plans): with them the frame loop never searches
    std::string err;
    const bool ok = kHw6Bundle_size && R.loadMaterialBundle(kHw6Bundle, kHw6Bundle_size, &err);
    std::fprintf(g_out, "# bundle %s entries=%zu %s\n", ok ? "loaded" : (kHw6Bundle_size ? "REFUSED" : "none"), R.bundleEntries(), err.c_str());
  }
  R.setFrameBeginHook(onFrameBegin);

  g_scene = "init";
  R.process(kHw6Stream + kHw6Scenes[0].off, kHw6Scenes[0].len);

  for (int s = 1; s <= kHw6NumScenes; s++) {
    if (s < kHw6NumScenes) {
      const Hw6Scene& S = kHw6Scenes[s];
      g_scene = S.name; g_copyIdx = 0;
      R.process(kHw6Stream + S.off, S.len);
      const uint8_t* next = s + 1 < kHw6NumScenes ? kHw6Scenes[s + 1].clear : kHw6Scenes[s].clear;
      const std::vector<uint8_t> x = hw6::xfbCopyBytes(next, true);
      R.process(x.data(), x.size());
    } else {
      // one more frame with nothing but the GXCopyDisp, so that the hook reads back the last scene
      g_scene = "end";
      const uint8_t black[4] = {0, 0, 0, 255};
      const std::vector<uint8_t> x = hw6::xfbCopyBytes(black, false);
      R.process(x.data(), x.size());
    }
    const t2p::FrameReport F = R.renderFrame();
    if (s < kHw6NumScenes) {
      g_pending.have = true; g_pending.name = kHw6Scenes[s].name;
      std::fprintf(g_out, "S\t%s\t%s\tdraws=%u drawn=%u refused=%u copies=%u/%u\tbuild %.2f ms plan %.2f ms draw %.2f ms\n", kHw6Scenes[s].name, F.rendered ? "rendered" : "NOT RENDERED",
                   unsigned(F.draws), unsigned(F.drawn), unsigned(F.refused), unsigned(F.copiesDone), unsigned(F.copies), F.buildMs, F.planMs, F.drawMs);
      std::fflush(g_out);
      std::printf("scene %d/%d %s\n", s, kHw6NumScenes - 1, kHw6Scenes[s].name);
    }
    gfxFlushBuffers(); gfxScreenSwapBuffers(GFX_BOTTOM, false);
  }

  const t2p::Backend3ds::Stats& bs = R.backend().stats();
  const t2p::TexCacheStats cs = R.cache().stats();
  std::fprintf(g_out, "# backend draws=%u refused=%u noArena=%u noCmdBuf=%u splits=%u shaders=%u textures=%u textureBytes=%llu arenaPeak=%llu stateSkips=%u copies=%u clears=%u\n", unsigned(bs.draws),
               unsigned(bs.refused), unsigned(bs.noArena), unsigned(bs.noCmdBuf), unsigned(bs.splits), unsigned(bs.shadersLoaded), unsigned(bs.texturesUploaded),
               static_cast<unsigned long long>(bs.textureBytes), static_cast<unsigned long long>(bs.arenaPeak), unsigned(bs.stateSkips), unsigned(bs.copies), unsigned(bs.clears));
  std::fprintf(g_out, "# texcache lookups=%llu hits=%llu misses=%llu conversions=%llu entries=%zu bytes=%zu frames=%llu\n", static_cast<unsigned long long>(cs.lookups),
               static_cast<unsigned long long>(cs.hits), static_cast<unsigned long long>(cs.misses), static_cast<unsigned long long>(cs.conversions), cs.entries, cs.bytes,
               static_cast<unsigned long long>(R.framesRendered()));
  const t2p::DrawCacheStats dcs = R.drawCache().stats();
  std::fprintf(g_out, "# drawcache lookups=%llu hits=%llu misses=%llu evictions=%llu entries=%zu\n", static_cast<unsigned long long>(dcs.lookups), static_cast<unsigned long long>(dcs.hits),
               static_cast<unsigned long long>(dcs.misses), static_cast<unsigned long long>(dcs.evictions), dcs.entries);
  {
    const t2p::BundleStats bst = t2p::bundleStats();
    std::fprintf(g_out, "# bundle lookups hits=%llu misses=%llu (searched %.1f ms)\n", static_cast<unsigned long long>(bst.hits), static_cast<unsigned long long>(bst.misses), bst.missSeconds * 1000.0);
  }
  std::fprintf(g_out, "# done\n");
  std::fclose(g_out); g_out = nullptr;
  std::fclose(g_frames); g_frames = nullptr;
  std::fclose(g_copies); g_copies = nullptr;
  std::printf("\nDone. Results: sdmc:/hwcheck7_result.txt, hwcheck7_frames.bin, hwcheck7_copies.bin\nSTART = exit.\n");
  while (aptMainLoop()) {
    hidScanInput();
    if (hidKeysDown() & KEY_START) break;
    gfxFlushBuffers(); gfxScreenSwapBuffers(GFX_BOTTOM, false); gspWaitForVBlank();
  }
  R.shutdown();
  C3D_Fini(); gfxExit();
  return 0;
}
