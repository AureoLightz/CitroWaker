// (Claude) N3DS_PROF_ORIGIN=1 diagnostic: GXCallDisplayList (linked with --wrap) processes the stream up to the
// raw display list, marks it origin 1 while it is processed, then back to the typed SDK calls (origin 0).
// Off (one call through) unless the env switch is set; meaningful with fifo_thread=0 (inline processing).
#include "fifo.hpp"
#include <cstdint>
extern "C" int n3dsOriginProfOn(void);
extern "C" void n3dsSetGxOrigin(int);
extern "C" void __real_GXCallDisplayList(const void* data, uint32_t nbytes);
// (Claude v92) N3DS_DL_CENSUS=1 diagnostic: while gpu_3ds.cpp has the census open (frames 600..700), every display list called is counted by its
// content (FNV-1a 64 of the bytes, size); dlcensus.txt lists them ("hash size calls"). Answers: how many of the lists drawn per frame are the
// model files' own lists (an offline geometry pack can only replace those).
#include <cstdio>
#include <cstring>
namespace {
struct DlCount { unsigned long long h; unsigned size, calls; };
DlCount g_dl[16384];
unsigned g_dlUsed = 0, g_dlOverflow = 0;
}
extern "C" { volatile int g_n3dsDlCensusOpen = 0; }
static void dlCensusCount(const void* data, uint32_t n) {
  unsigned long long h = 0xCBF29CE484222325ull; const unsigned char* p = static_cast<const unsigned char*>(data);
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
  unsigned k = unsigned(h ^ (h >> 29)) & 16383;
  for (unsigned probe = 0; probe < 16384; probe++, k = (k + 1) & 16383) {
    if (g_dl[k].calls == 0) { if (g_dlUsed >= 15000) { g_dlOverflow++; return; } g_dl[k] = {h, n, 1}; g_dlUsed++; return; }
    if (g_dl[k].h == h && g_dl[k].size == n) { g_dl[k].calls++; return; }
  }
}
// (Claude v97 diag) the same census by caller (return address of GXCallDisplayList): calls and bytes; dlcallers.txt next to dlcensus.txt
namespace { struct DlCaller { const void* ra; unsigned calls; unsigned long long bytes; }; DlCaller g_dlc[1024]; }
static void dlCallerCount(const void* ra, uint32_t n) {
  unsigned k = unsigned(reinterpret_cast<uintptr_t>(ra) >> 2) & 1023;
  for (unsigned probe = 0; probe < 1024; probe++, k = (k + 1) & 1023) {
    if (g_dlc[k].calls == 0) { g_dlc[k] = {ra, 1, n}; return; }
    if (g_dlc[k].ra == ra) { g_dlc[k].calls++; g_dlc[k].bytes += n; return; }
  }
}
extern "C" void n3dsDlCallersWrite(const char* path) {
  if (FILE* f = std::fopen(path, "w")) {
    for (const DlCaller& d : g_dlc) if (d.calls) std::fprintf(f, "%p %u %llu\n", d.ra, d.calls, d.bytes);
    std::fclose(f);
  }
}
// (Claude v102 diag) GXBegin callers during the census window (immediate-mode draws: particles, sea, shadows, 2D): calls and vertices -> gxbegin.txt
namespace { struct BeginCaller { const void* ra; unsigned calls; unsigned long long verts; }; BeginCaller g_bc[1024]; }
extern "C" void __real_GXBegin(int primitive, int vtxFmt, unsigned short nVerts);
extern "C" void __wrap_GXBegin(int primitive, int vtxFmt, unsigned short nVerts) {
  if (g_n3dsDlCensusOpen) {
    const void* ra = __builtin_return_address(0);
    unsigned k = unsigned(reinterpret_cast<uintptr_t>(ra) >> 2) & 1023;
    for (unsigned probe = 0; probe < 1024; probe++, k = (k + 1) & 1023) {
      if (g_bc[k].calls == 0) { g_bc[k] = {ra, 1, nVerts}; break; }
      if (g_bc[k].ra == ra) { g_bc[k].calls++; g_bc[k].verts += nVerts; break; }
    }
  }
  __real_GXBegin(primitive, vtxFmt, nVerts);
}
extern "C" void n3dsDlCensusWrite(const char* path) {
  { char p3[256]; std::snprintf(p3, sizeof p3, "%.*sgxbegin.txt", int(std::strrchr(path, '/') ? std::strrchr(path, '/') - path + 1 : 0), path);
    if (FILE* f = std::fopen(p3, "w")) { for (const BeginCaller& b : g_bc) if (b.calls) std::fprintf(f, "%p %u %llu\n", b.ra, b.calls, b.verts); std::fclose(f); } }
  { char p2[256]; std::snprintf(p2, sizeof p2, "%.*sdlcallers.txt", int(std::strrchr(path, '/') ? std::strrchr(path, '/') - path + 1 : 0), path); n3dsDlCallersWrite(p2); }
  if (FILE* f = std::fopen(path, "w")) {
    for (const DlCount& d : g_dl) if (d.calls) std::fprintf(f, "%016llx %u %u\n", d.h, d.size, d.calls);
    std::fprintf(f, "# overflow %u\n", g_dlOverflow); std::fclose(f);
  }
}
extern "C" void __wrap_GXCallDisplayList(const void* data, uint32_t nbytes) {
  if (g_n3dsDlCensusOpen) { dlCensusCount(data, nbytes); dlCallerCount(__builtin_return_address(0), nbytes); }
  if (!n3dsOriginProfOn()) { __real_GXCallDisplayList(data, nbytes); return; }
  aurora::gx::fifo::drain();
  n3dsSetGxOrigin(1);
  __real_GXCallDisplayList(data, nbytes);
  aurora::gx::fifo::drain();
  n3dsSetGxOrigin(0);
}

// (Claude) N3DS_PROF_WAIT=1 diagnostic: time the calling thread spends inside fifo::drain() / fifo::sync() (linked with --wrap; calls from
// other files only). Read and cleared by aurora_3ds.cpp's [game-wait] line.
#include <cstdlib>
extern "C" unsigned long long svcGetSystemTick(void);
extern "C" { unsigned long long g_n3dsDrainTicks = 0, g_n3dsSyncTicks = 0; unsigned g_n3dsDrainCalls = 0, g_n3dsSyncCalls = 0; }
extern "C" void __real__ZN6aurora2gx4fifo5drainEv(void);
extern "C" void __real__ZN6aurora2gx4fifo4syncEv(void);
static bool waitProfOn() { static const bool on = std::getenv("N3DS_PROF_WAIT") != nullptr; return on; }
extern "C" { extern volatile unsigned g_n3dsMark[8]; }
// (Claude v97 diag) who drains: callers of drain() and of GXDrawDone (game side), counted while N3DS_PROF_WAIT is set; [drain-callers] lines
namespace { struct RaCount { const void* ra; unsigned n; unsigned long long ticks; }; RaCount g_drainRa[16], g_ddRa[16]; }
static void raCount(RaCount* t, const void* ra, unsigned long long ticks) {
  for (int i = 0; i < 16; i++) { if (t[i].ra == ra) { t[i].n++; t[i].ticks += ticks; return; } if (!t[i].ra) { t[i] = {ra, 1, ticks}; return; } }
}
extern "C" void n3dsLog(const char* fmt, ...);
extern "C" void n3dsDrainCallersLog(void) {
  for (int i = 0; i < 16; i++) if (g_drainRa[i].ra) n3dsLog("[drain-callers] drain from %p: %u calls %.1f ms\n", g_drainRa[i].ra, g_drainRa[i].n, g_drainRa[i].ticks / 268111.856);
  for (int i = 0; i < 16; i++) if (g_ddRa[i].ra) n3dsLog("[drain-callers] GXDrawDone from %p: %u calls %.1f ms\n", g_ddRa[i].ra, g_ddRa[i].n, g_ddRa[i].ticks / 268111.856);
  for (int i = 0; i < 16; i++) g_drainRa[i] = g_ddRa[i] = RaCount{};
}
extern "C" void __real_GXDrawDone(void);
extern "C" void __wrap_GXDrawDone(void) {
  if (!waitProfOn()) { __real_GXDrawDone(); return; }
  const unsigned long long t = svcGetSystemTick();
  __real_GXDrawDone();
  raCount(g_ddRa, __builtin_return_address(0), svcGetSystemTick() - t);
}
extern "C" void __wrap__ZN6aurora2gx4fifo5drainEv(void) {
  if (!waitProfOn()) { g_n3dsMark[0] = g_n3dsMark[0] + 1; __real__ZN6aurora2gx4fifo5drainEv(); g_n3dsMark[1] = g_n3dsMark[1] + 1; return; }
  const unsigned long long t = svcGetSystemTick();
  __real__ZN6aurora2gx4fifo5drainEv();
  raCount(g_drainRa, __builtin_return_address(0), svcGetSystemTick() - t);
  g_n3dsDrainTicks += svcGetSystemTick() - t; g_n3dsDrainCalls++;
}
extern "C" void __wrap__ZN6aurora2gx4fifo4syncEv(void) {
  if (!waitProfOn()) { __real__ZN6aurora2gx4fifo4syncEv(); return; }
  const unsigned long long t = svcGetSystemTick();
  __real__ZN6aurora2gx4fifo4syncEv();
  g_n3dsSyncTicks += svcGetSystemTick() - t; g_n3dsSyncCalls++;
}

// (Claude) Console diagnosis of a GPU that never finishes a frame: the GX command queue citro3d binds and the last command lists handed to
// GX_ProcessCommandList are remembered (linked with --wrap), for the watchdog in gpu_3ds.cpp to log and save when frames stop.
extern "C" {
void* g_n3dsGxQueue = nullptr;
unsigned* g_n3dsCmdListAddr[4] = {};
unsigned g_n3dsCmdListSize[4] = {};
unsigned g_n3dsCmdListCount = 0;
void __real_GX_BindQueue(void* queue);
void __wrap_GX_BindQueue(void* queue) { if (queue) g_n3dsGxQueue = queue; __real_GX_BindQueue(queue); }
int __real_GX_ProcessCommandList(unsigned* addr, unsigned size, unsigned char flags);
int __wrap_GX_ProcessCommandList(unsigned* addr, unsigned size, unsigned char flags) {
  const unsigned i = g_n3dsCmdListCount & 3;
  g_n3dsCmdListAddr[i] = addr; g_n3dsCmdListSize[i] = size; g_n3dsCmdListCount = g_n3dsCmdListCount + 1;
  return __real_GX_ProcessCommandList(addr, size, flags);
}
}
