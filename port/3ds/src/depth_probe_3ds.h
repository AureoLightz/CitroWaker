// (Claude v85) Console depth probe: what the PICA actually stores for known depth-map settings. v83 and v84 read 0x000000 / 0x9C4000 and
// 0x800000 / 0x7FFFFF on the console where Azahar stores 0xFFFFFF / 0xFFBFFF, so the emulator's depth model cannot be trusted for this.
// Runs once at boot on a private 64x64 RGBA8 + D24S8 target (well under a second), logs "[depth-probe]" lines and frees everything.
#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "picaenc.h"
#include "n3ds.h"
extern "C" void C3Di_RenderQueueWaitDone(void);
extern "C" void C3Di_UpdateContext(void);
namespace depthprobe {
struct V { float p[4]; };
inline uint32_t f24(float f) { uint32_t i; std::memcpy(&i, &f, 4); const uint32_t sg = i >> 31; if (!(i & 0x7FFFFFFF)) return sg << 23;
  int e = int((i >> 23) & 0xFF) - 127 + 63; if (e <= 0) return sg << 23; if (e > 127) e = 127; return sg << 23 | uint32_t(e) << 16 | ((i & 0x7FFFFF) >> 7); }
inline unsigned tiled(unsigned x, unsigned y) { unsigned m = 0; for (unsigned b = 0; b < 3; ++b) m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1); return ((y >> 3) * 8 + (x >> 3)) * 64 + m; }
inline void run() {
  auto* target = C3D_RenderTargetCreate(64, 64, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
  const char* src = ".constf white(1,1,1,1)\n.in p v0\n.out op position\n.out oc color\n.proc main\n mov op,p\n mov oc,white\n end\n.end\n";
  auto bin = t2p::assemblePica(src, "depthprobe.v.pica");
  // (Claude v88) the program and its DVLB live for the whole process: citro3d keeps the last bound program (ctx->program) and C3D_BindProgram reads
  // oldProg->vertexShader->dvle->dvlp on the next bind. v85-v87 freed this stack-local program, so the game's first draw read freed memory: a null-pointer
  // data abort in C3D_BindProgram on the console (the emulator happened to keep the old bytes). A few hundred bytes, never freed.
  // DVLB_ParseFile points into the SHBIN bytes (no copy): they stay too.
  static DVLB_s* dv = nullptr; static shaderProgram_s program{}; static std::vector<uint8_t> shbin;
  if (!dv) { shbin = bin.shbin; dv = bin.ok ? DVLB_ParseFile(reinterpret_cast<u32*>(shbin.data()), shbin.size()) : nullptr; shaderProgramInit(&program); }
  const bool shaderOK = dv && R_SUCCEEDED(shaderProgramSetVsh(&program, &dv->DVLE[0]));
  auto* verts = static_cast<V*>(linearAlloc(6 * sizeof(V)));
  if (!target || !shaderOK || !verts) { n3dsLog("[depth-probe] setup failed\n"); }
  else {
    // corners: (x,y) = (-1,-1) (1,-1) (1,1) / (-1,-1) (1,1) (-1,1); zl/wl on the left edge (x=-1), zr/wr on the right edge
    auto draw = [&](const char* label, bool zbuf, float scale, float offset, float zl, float wl, float zr, float wr, bool doDraw, uint32_t expect) {
      const float cx[6] = {-1, 1, 1, -1, 1, -1}, cy[6] = {-1, -1, 1, -1, 1, 1};
      for (int i = 0; i < 6; i++) { const bool r = cx[i] > 0; const float w = r ? wr : wl, z = r ? zr : zl; verts[i] = V{{cx[i] * w, cy[i] * w, z, w}}; }
      GSPGPU_FlushDataCache(verts, 6 * sizeof(V));
      C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
      C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0x123456);
      C3D_FrameDrawOn(target);
      if (doDraw) {
        C3D_BindProgram(&program);
        auto* ai = C3D_GetAttrInfo(); AttrInfo_Init(ai); AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
        auto* bi = C3D_GetBufInfo(); BufInfo_Init(bi); BufInfo_Add(bi, verts, sizeof(V), 1, 0x0);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0); C3D_CullFace(GPU_CULL_NONE);
        C3D_DepthMap(zbuf, scale, offset); C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
        C3D_AlphaTest(false, GPU_ALWAYS, 0); C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false); C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
        for (int i = 0; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
        auto* env = C3D_GetTexEnv(0); C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR); C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
      }
      GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1); C3D_FrameEnd(0); C3Di_RenderQueueWaitDone();
      const auto* d = static_cast<const uint32_t*>(target->frameBuf.depthBuf);
      uint32_t lo = 0xFFFFFFFFu, hi = 0;
      for (unsigned i = 0; i < 64 * 64; i++) { const uint32_t v = d[i] & 0xFFFFFF; lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
      char line[400];
      // two 8-sample lines across the target (which axis is x on screen is part of what this shows)
      char a[128] = {}, b[128] = {}; int pa = 0, pb = 0;
      for (unsigned i = 4; i < 64; i += 8) {
        pa += std::snprintf(a + pa, sizeof a - pa, " %06lx", (unsigned long)(d[tiled(i, 32)] & 0xFFFFFF));
        pb += std::snprintf(b + pb, sizeof b - pb, " %06lx", (unsigned long)(d[tiled(32, i)] & 0xFFFFFF));
      }
      std::snprintf(line, sizeof line, "[depth-probe] %-26s %c scale %.9g offset %.9g: centre %06lx min %06lx max %06lx stencil %02lx expect %06lx | a%s | b%s\n",
                    label, zbuf ? 'Z' : 'W', double(scale), double(offset), (unsigned long)(d[tiled(32, 32)] & 0xFFFFFF), (unsigned long)lo, (unsigned long)hi,
                    (unsigned long)(d[tiled(32, 32)] >> 24), (unsigned long)expect, a, b);
      n3dsLog("%s", line);
    };
    auto e = [](double v) { return uint32_t(v * 16777215.0 + 0.5); };
    n3dsLog("[depth-probe] begin: fill 0x123456 then one full-target quad, depth ALWAYS + write; raw = low 24 bits of D24S8\n");
    draw("fill only", true, 0, 0, -0.5f, 1, -0.5f, 1, false, 0x123456);
    const float kc[] = {0.25f, 0.5f, 0.75f, 255.0f / 256.0f, 1023.0f / 1024.0f, 1.0f};
    for (float c : kc) { char l[40]; std::snprintf(l, sizeof l, "Z const %.6f", double(c)); draw(l, true, 0, c, -0.5f, 1, -0.5f, 1, true, e(c)); }
    const float zs[] = {-1.0f, -0.75f, -0.5f, -0.25f, -0.01f, 0.0f};
    for (float z : zs) { char l[40]; std::snprintf(l, sizeof l, "Z s1 o1 z/w %.2f", double(z)); draw(l, true, 1, 1, z, 1, z, 1, true, e(z + 1)); }
    draw("Z s1 o1 w100 z-50", true, 1, 1, -50, 100, -50, 100, true, e(0.5));
    draw("Z k,k z/w -0.5", true, 255.0f / 256, 255.0f / 256, -0.5f, 1, -0.5f, 1, true, e(0.5 * 255 / 256));
    draw("Z k,k z/w 0", true, 255.0f / 256, 255.0f / 256, 0, 1, 0, 1, true, e(255.0 / 256));
    const float ws[] = {250, 500, 750, 996, 1000};
    for (float w : ws) { char l[40]; std::snprintf(l, sizeof l, "W 1/1000 w %.0f", double(w)); draw(l, false, 0, 1.0f / 1000, -0.5f * w, w, -0.5f * w, w, true, e(w / 1000)); }
    draw("W k/1000 w 500", false, 0, (255.0f / 256) / 1000, -250, 500, -250, 500, true, e(0.5 * 255 / 256));
    draw("W k/1000 w 1000", false, 0, (255.0f / 256) / 1000, -500, 1000, -500, 1000, true, e(255.0 / 256));
    draw("W 1/160000 w 80000", false, 0, 1.0f / 160000, -40000, 80000, -40000, 80000, true, e(0.5));
    draw("W 1/160000 w 4000", false, 0, 1.0f / 160000, -2000, 4000, -2000, 4000, true, e(4000.0 / 160000));
    draw("W s1/1000 o0 z-500 w1000", false, 1.0f / 1000, 0, -500, 1000, -500, 1000, true, e(0.5));
    // (Claude v93) sweep with the registers at the citro3d defaults (-1, 0), which is all the console honours: Z mode should store -z/w; what W mode
    // stores is the open question (v92: ~1/100 of Azahar's values in game). "expect" = -z/w (Z) and -z (W), the documented formulas.
    { const float zs2[] = {-0.0001f, -0.001f, -0.01f, -0.05f, -0.1f, -0.25f, -0.5f, -0.75f, -0.99f}; const float ws2[] = {1, 4, 16, 100, 1000};
      for (float w : ws2) for (float zr : zs2) { char l[48];
        std::snprintf(l, sizeof l, "S Z z/w %g w %g", double(zr), double(w)); draw(l, true, -1, 0, zr * w, w, zr * w, w, true, e(-zr));
        std::snprintf(l, sizeof l, "S W -z %g w %g", double(-zr * w), double(w)); draw(l, false, -1, 0, zr * w, w, zr * w, w, true, e(-zr * w)); } }
    draw("Z grad z/w -1..0", true, 1, 1, -1, 1, 0, 1, true, 0);
    draw("W grad w 100..1000", false, 0, 1.0f / 1000, -50, 100, -500, 1000, true, 0);
    draw("Z grad persp w 100..1000", true, 1, 1, -90, 100, -100, 1000, true, 0);
    n3dsLog("[depth-probe] end\n");
    // (Claude v89) probe 2: the console ignored every C3D_DepthMap scale/offset above (all values = the citro3d defaults -1 / 0) while
    // DEPTHMAP_ENABLE took effect. Each variant draws z/w -0.5 (defaults store 800000) asking for scale 0 offset 0.75 (bfffff), then reads the PICA's
    // own registers 0x4D / 0x4E / 0x6D back through GSP (u32 at 0x1EF01000 + 4 * reg).
    auto variant = [&](const char* label, int v) {
      const float sc = 0.0f, of = 0.75f;
      for (int i = 0; i < 6; i++) verts[i] = V{{(i == 1 || i == 2 || i == 4) ? 1.0f : -1.0f, (i == 2 || i == 4 || i == 5) ? 1.0f : -1.0f, -0.5f, 1.0f}};
      GSPGPU_FlushDataCache(verts, 6 * sizeof(V));
      C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
      C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0x123456);
      C3D_FrameDrawOn(target);
      C3D_BindProgram(&program);
      auto* ai = C3D_GetAttrInfo(); AttrInfo_Init(ai); AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
      auto* bi = C3D_GetBufInfo(); BufInfo_Init(bi); BufInfo_Add(bi, verts, sizeof(V), 1, 0x0);
      C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0); C3D_CullFace(GPU_CULL_NONE);
      C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
      C3D_AlphaTest(false, GPU_ALWAYS, 0); C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
      C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false); C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
      for (int i = 0; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
      auto* env = C3D_GetTexEnv(0); C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR); C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
      const bool api = v == 0 || v == 5;
      C3D_DepthMap(true, api ? sc : -1.0f, api ? of : 0.0f);
      C3Di_UpdateContext();
      const uint32_t fs = f24(sc), fo = f24(of);
      if (v == 1 || v == 3 || v == 4) { GPUCMD_AddWrite(GPUREG_DEPTHMAP_SCALE, fs); GPUCMD_AddWrite(GPUREG_DEPTHMAP_OFFSET, fo); }
      if (v == 2) { GPUCMD_AddWrite(GPUREG_DEPTHMAP_OFFSET, fo); GPUCMD_AddWrite(GPUREG_DEPTHMAP_SCALE, fs); }
      if (v == 3) GPUCMD_AddWrite(GPUREG_DEPTHMAP_ENABLE, 1);
      if (v == 4) { C3D_SetViewport(0, 0, 64, 64); C3Di_UpdateContext(); }
      C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
      if (v == 5) C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
      GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1); C3D_FrameEnd(0); C3Di_RenderQueueWaitDone();
      const auto* d = static_cast<const uint32_t*>(target->frameBuf.depthBuf);
      uint32_t regs[2] = {0xDEAD, 0xDEAD}, en = 0xDEAD;
      const Result r1 = GSPGPU_ReadHWRegs(0x401000 + 4 * GPUREG_DEPTHMAP_SCALE, regs, 8), r2 = GSPGPU_ReadHWRegs(0x401000 + 4 * GPUREG_DEPTHMAP_ENABLE, &en, 4);
      n3dsLog("[depth-probe2] %-34s centre %06lx corner %06lx (defaults 800000, wanted bfffff) | wrote scale %06lx offset %06lx | read 4D %08lx 4E %08lx 6D %08lx (rc %08lx %08lx)\n",
              label, (unsigned long)(d[tiled(32, 32)] & 0xFFFFFF), (unsigned long)(d[tiled(3, 3)] & 0xFFFFFF), (unsigned long)fs, (unsigned long)fo,
              (unsigned long)regs[0], (unsigned long)regs[1], (unsigned long)en, (unsigned long)r1, (unsigned long)r2);
    };
    variant("api C3D_DepthMap", 0);
    variant("late single writes scale,offset", 1);
    variant("late single writes offset,scale", 2);
    variant("late writes + DEPTHMAP_ENABLE after", 3);
    variant("late writes + viewport after", 4);
    variant("api, drawn twice", 5);
    n3dsLog("[depth-probe2] end\n");
  }
  if (verts) linearFree(verts);   // only read by the draws above (C3Di_RenderQueueWaitDone after each); the next draw binds its own buffers
  if (target) C3D_RenderTargetDelete(target);
}
}  // namespace depthprobe
