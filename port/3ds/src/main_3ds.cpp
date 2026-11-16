// Entry point of the 3DS build: screens and log first, then the game's own main (m_Do_main.cpp, renamed
// aurora_main by <aurora/main.h>) with the settings the PC harness reads from the environment.
#include <cstdint>
namespace t2p { extern void (*g_drawCacheMissLog)(const char*); extern uint64_t (*g_compileClock)(); }
#include <cstddef>
extern void (*g_texConvLog)(uint32_t, int, int, int, size_t, bool, int, uint64_t);
#include <3ds.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include "n3ds.h"
#include "core2_replay_test_3ds.h"

extern "C" int aurora_main(int argc, char* argv[]);
extern "C" void aurora_shutdown(void);
namespace { bool s_gameStarted=false; }
// libctru calls this before closing FS/APT services on _Exit. The GameCube
// background threads are not destructed; their stacks must remain mapped.
extern "C" void userAppExit(void){
  if(!n3dsPrepareExit())return;
  // A bounded escape if a damaged GPU prevents owner-thread cleanup.
  threadCreate([](void*){svcSleepThread(10000000000LL);svcExitProcess();},nullptr,0x2000,0x19,0,true);
  n3dsLog("[exit] cleanup begin game %d\n",int(s_gameStarted));
  if(s_gameStarted){
    const int cardIdle=n3dsMemcardQuiesce(5000);
    n3dsLog("[exit] card idle %d\n",cardIdle);
    n3dsAudioShutdown();n3dsLog("[exit] NDSP stopped\n");
    aurora_shutdown();n3dsLog("[exit] FIFO/render owner/GPU stopped\n");
  }
  gfxExit();n3dsLog("[exit] gfx stopped; services next, heaps stay mapped\n");
}
// (Claude v82) libctru's __appExit (userAppExit, archiveUnmountAll, fsExit, hidExit, aptExit, srvExit) with a log line per step and the
// threads still alive, so a console that fails while quitting shows the step. aptExit runs before the card closes so its result is logged.
static void exitThreadReport(void){
  u32 ids[48];s32 n=0;
  if(R_FAILED(svcGetThreadList(&n,ids,48,CUR_PROCESS_HANDLE))||n<0||n>48){n3dsLog("[exit] thread list unavailable\n");return;}
  u32 self=0;svcGetThreadId(&self,CUR_THREAD_HANDLE);
  n3dsLog("[exit] %ld threads alive (this one %lu):",(long)n,(unsigned long)self);
  for(s32 i=0;i<n&&i<48;i++){
    Handle h=0;s32 core=-9,prio=-1;
    if(R_SUCCEEDED(svcOpenThread(&h,CUR_PROCESS_HANDLE,ids[i]))){svcGetThreadIdealProcessor(&core,h);svcGetThreadPriority(&prio,h);svcCloseHandle(h);}
    n3dsLog(" %lu/c%ld/p%lx",(unsigned long)ids[i],(long)core,(long)prio);
  }
  n3dsLog("\n");
}
extern "C" void __appExit(void){
  static volatile int entered=0;
  u32 caller=0;svcGetThreadId(&caller,CUR_THREAD_HANDLE);
  if(__atomic_exchange_n(&entered,1,__ATOMIC_ACQ_REL)){n3dsLog("[exit] second exit request from thread %lu ignored\n",(unsigned long)caller);for(;;)svcSleepThread(1000000000LL);}
  n3dsLog("[exit] exit path on thread %lu\n",(unsigned long)caller);
  userAppExit();
  exitThreadReport();
  n3dsLog("[exit] aptExit next\n");
  aptExit();
  // (Claude v84) v83 on the console logged every step up to here and still needed a reset: 21 game threads (cores 0-2) were alive while HID, the card
  // and srv closed under them. A thread that touches a closed service ends in a fatal error. APT has closed the application, so nothing else needs an
  // orderly close: the kernel ends every thread and closes every handle in one step.
  // (Claude v113) HOME-close fix. libctru's aptExit does NOT close the application: on a system close it only calls
  // APT_PrepareToCloseApplication and leaves APT_CloseApplication in __system_retAddr for libctru's __libctru_exit to call last.
  // Our exit (v84+) ended the process before that, so the HOME Menu saw the application vanish without closing:
  // "An error has occurred, forcing the software to close. The system will now restart" on every close.
  extern void (*__system_retAddr)(void);
  n3dsLog("[exit] apt prepared; APT_CloseApplication %s, then the process ends (kernel closes hid, card, srv)\n",__system_retAddr?"next":"not requested");
  n3dsCloseLog();
  if(__system_retAddr)__system_retAddr();   // APT_CloseApplication: the system ends the process from here
  svcExitProcess();
}
extern "C" [[noreturn]] void __libctru_exit(int){
  // CIA-only delivery: let the kernel stop every thread before releasing its
  // stack. libctru's default unmaps the live pthread stacks first (dump12).
  // (Claude v113) but still make the APT close libctru's default makes last (see __appExit)
  extern void (*__system_retAddr)(void);
  if(__system_retAddr)__system_retAddr();
  svcExitProcess();
  __builtin_unreachable();
}
extern "C" size_t g_bigAllocLimit;
extern "C" void n3dsAllocSitesEnable();
namespace { extern bool g_bigLog; }

u32 __stacksize__ = 0x200000;   // main01 runs on the main thread (2 MiB, as deep as the PC's)
u32 __ctru_linear_heap_size = 18u << 20;   // GPU-visible memory (command lists, vertex arena, textures); the rest of the application memory is heap

// (Claude) libctru's default heap takes every remaining byte of the application's memory, so the kernel has none left for what a process
// commits later (the console refused the game's 14th thread: log-cia-run3). Same as libctru's __system_allocateHeaps, 18 MiB linear heap,
// but 1 MiB is left uncommitted.
extern "C" {
// Request speedup from the earliest APT initialization. n3dsInit also calls
// osSetSpeedupEnable(true); this default is not a measured gameplay FPS fix.
bool __ctru_speedup = true;
extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap, __ctru_linear_heap, __ctru_heap_size;
void __system_allocateHeaps(void) {
  Handle reslimit = 0;
  if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
  s64 maxCommit = 0, currentCommit = 0;
  ResourceLimitType type = RESLIMIT_COMMIT;
  svcGetResourceLimitLimitValues(&maxCommit, reslimit, &type, 1);
  svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &type, 1);
  svcCloseHandle(reslimit);
  const u32 remaining = u32(maxCommit - currentCommit) & ~0xFFFu;
  const u32 slack = 1u << 20;
  if (remaining < __ctru_linear_heap_size + slack + (16u << 20)) svcBreak(USERBREAK_PANIC);
  __ctru_heap_size = remaining - __ctru_linear_heap_size - slack;
  if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC, MemPerm(MEMPERM_READ | MEMPERM_WRITE)))) svcBreak(USERBREAK_PANIC);
  if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, MemPerm(MEMPERM_READ | MEMPERM_WRITE)))) svcBreak(USERBREAK_PANIC);
  mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
  fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
  fake_heap_end = fake_heap_start + __ctru_heap_size;
}
}

int main(int, char**) {
  n3dsInit();
  if(core2test::requested()) return core2test::run();   // (Claude v62) one-shot: only with sdmc:/3ds/ww3ds/claude-replay/config.txt, which it deletes
#ifdef N3DS_RENDER_OPT
  n3dsLog("[build] renderer compiler O%d LTO %d, strict floating point\n",N3DS_RENDER_OPT,N3DS_RENDER_LTO);
#endif
  bool cpuIsNew=false;
  const Result cpuModelResult=APT_CheckNew3DS(&cpuIsNew);
  if(R_SUCCEEDED(cpuModelResult) && cpuIsNew){
    const Result cpuServiceResult=ptmSysmInit();
    const Result cpuConfigureResult=R_SUCCEEDED(cpuServiceResult)?PTMSYSM_ConfigureNew3DSCPU(3):cpuServiceResult;
    if(R_SUCCEEDED(cpuServiceResult))ptmSysmExit();
    n3dsLog("[cpu-mode] New3DS speedup 1 requested clock/L2 3 service %08lx configure %08lx\n",(unsigned long)cpuServiceResult,(unsigned long)cpuConfigureResult);
  }else n3dsLog("[cpu-mode] New3DS check %08lx result %d\n",(unsigned long)cpuModelResult,int(cpuIsNew));

  n3dsLog("[build] CitroWaker v128 (Claude): fast allocator capped at 64 spans (4 MB) so long sessions keep heap for draw replay. v127: runtime textures; fractional pacing and rigid clip OFF by default: full-width telescope covers; replay cache trims under pressure; prior v122: matrix groups replay from the previous frame, texture checks once per frame, resident geometry by pointer, interned uniform names, compile-cache hash halved, matrix slot memo, moving collision skips plane re-classification on pure translations. v121: <=2 logic ticks per painted frame, no catch-up bursts, present hold off\n");
  // (Claude) The game needs the New 3DS's 124 MB mode (the CIA's exheader asks for it; the HOME Menu applies it when it starts the title).
  // Started some other way the console can stay in the 64 MB mode, where the game's memory does not fit: relaunch this title once through
  // APT (the system switches modes when it has to), and if that did not help either, say so on the screen instead of freezing.
  {
    static const char* marker = N3DS_ROOT "/relaunch.tmp";
    const u32 appMem = osGetMemRegionSize(MEMREGION_APPLICATION);
    struct stat ms;
    const bool tried = stat(marker, &ms) == 0;
    if (appMem >= (90u << 20))   /* Azahar's .3dsx: 96 MB, enough */ { if (tried) remove(marker); }
    else {
      u64 tid = 0;
      const Result idr = APT_GetProgramID(&tid);
      if (!tried && R_SUCCEEDED(idr) && (tid >> 32) == 0x00040000) {
        if (FILE* f = std::fopen(marker, "w")) std::fclose(f);
        n3dsLog("[3ds] only %lu KB of application memory: relaunching title %016llx once for the 124 MB mode\n", (unsigned long)(appMem >> 10), (unsigned long long)tid);
        // A full reboot into the title makes the system apply its memory mode (an APT application jump, what FBI's "launch" uses, keeps the
        // current 64 MB mode). _exit, not return: the game's global destructors must not run before the game was ever set up (they crash).
        if (R_SUCCEEDED(nsInit())) {
          const Result rr = NS_RebootToTitle(MEDIATYPE_SD, tid, 6);   // application memory type 6 = the New 3DS 124 MB layout
          n3dsLog("[3ds] NS_RebootToTitle %08lx\n", (unsigned long)rr);
          if (R_SUCCEEDED(rr)) for (;;) svcSleepThread(100000000LL);
          nsExit();
        } else n3dsLog("[3ds] ns:s not available\n");
        aptSetChainloader(tid, MEDIATYPE_SD);
        gfxExit();
        _exit(0);
      }
      remove(marker);
      n3dsLog("\nNot enough memory: the console gave this app %lu MB,\nit needs the New 3DS 124 MB mode.\n\nStart it from its HOME Menu icon (not from FBI or\nanother launcher). If this was the HOME Menu icon,\nturn the console fully off and on and try again.\n\nPress START to exit.\n", (unsigned long)(appMem >> 20));
      while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
      }
      gfxExit();
      _exit(1);
    }
  }
  // testing: N3DS_ROOT/env.txt, KEY=VALUE lines set before the defaults below (e.g. COS_BOOT_STAGE=sea:44:206). Absent for players.
  if (FILE* f = std::fopen(N3DS_ROOT "/env.txt", "r")) {
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
      char* eq = std::strchr(line, '=');
      if (!eq || line[0] == '#') continue;
      *eq = 0;
      char* v = eq + 1;
      v[std::strcspn(v, "\r\n")] = 0;
      setenv(line, v, 1);
      n3dsLog("[3ds] env %s=%s\n", line, v);
    }
    std::fclose(f);
  }
  if (const char* v = std::getenv("N3DS_BIGALLOC_KB")) g_bigAllocLimit = size_t(std::atoi(v)) << 10;
  if (const char* v = std::getenv("N3DS_ALLOC_SITES")) if (*v == '1') n3dsAllocSitesEnable();
  if (const char* v = std::getenv("N3DS_DC_DIAG")) if (*v == '1') { t2p::g_drawCacheMissLog = [](const char* m) { n3dsLog("%s\n", m); }; t2p::g_compileClock = []() -> uint64_t { return svcGetSystemTick(); };
    g_texConvLog = [](uint32_t a, int f, int w, int h, size_t n, bool pack, int stale, uint64_t t) { n3dsLog("[tex-conv] addr %08lx fmt %d %dx%d %u B pack %d why %d %.2f ms\n", (unsigned long)a, f, w, h, unsigned(n), int(pack), int(stale), double(t) * 1000.0 / SYSCLOCK_ARM11); }; }   // (Claude v105 diag) compile-miss key diffs
  if (const char* v = std::getenv("N3DS_BIGALLOC_LOG")) g_bigLog = std::atoi(v) != 0;
  setenv("COS_DISC", N3DS_ROOT "/GZLE01.iso", 0);
  setenv("COS_PERF_LOG", "0", 0);
  setenv("COS_FPS60", "0", 0);
  mkdir(N3DS_ROOT "/user", 0777);
  struct stat st;
  if (stat(N3DS_ROOT "/GZLE01.iso", &st) != 0) {
    n3dsLog("\nPut the Wind Waker (USA) disc image at\n  %s/GZLE01.iso\n\nPress START to exit.\n", N3DS_ROOT);
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_START) break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    gfxExit();
    _exit(1);   // (not return: the game's global destructors crash when the game never started)
  }
  static char arg0[] = N3DS_ROOT "/ww3ds.3dsx";
  char* argv[] = {arg0, nullptr};
  s_gameStarted=true;
  return aurora_main(1, argv);
}

// ---- testing aid: N3DS_BIGALLOC_KB=n in env.txt tracks every live host allocation of at least n KB (linked with --wrap=malloc etc.) and
// n3dsBigAllocReport() logs the call sites holding the most; N3DS_BIGALLOC_LOG=1 also logs each one. operator new reports its own caller
// (n3dsNoteCaller, pc_jkr_heap.cpp).
#include <cstdlib>
extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_aligned_alloc(size_t, size_t);
void __real_free(void*);
size_t g_bigAllocLimit = 0;   // set by main from N3DS_BIGALLOC_KB once env.txt is read
}
namespace {
bool g_bigLog = false;
struct Live { void* p; size_t n; void* caller; };
constexpr unsigned kSlots = 1u << 15;
Live g_live[kSlots];          // open addressing on the pointer; p == 1: deleted
LightLock g_liveLock;
bool g_liveLockInit = false;
unsigned slotOf(void* p) { return (uintptr_t(p) >> 3) * 2654435761u >> 17; }
struct Guard { Guard() { if (!g_liveLockInit) { LightLock_Init(&g_liveLock); g_liveLockInit = true; } LightLock_Lock(&g_liveLock); } ~Guard() { LightLock_Unlock(&g_liveLock); } };
void liveAdd(void* p, size_t n, void* caller) {
  Guard g;
  for (unsigned i = slotOf(p), k = 0; k < kSlots; k++, i = (i + 1) & (kSlots - 1))
    if (g_live[i].p == nullptr || g_live[i].p == (void*)1) { g_live[i] = Live{p, n, caller}; return; }
}
void liveDel(void* p) {
  Guard g;
  for (unsigned i = slotOf(p), k = 0; k < kSlots; k++, i = (i + 1) & (kSlots - 1)) {
    if (g_live[i].p == nullptr) return;
    if (g_live[i].p == p) { g_live[i].p = (void*)1; return; }
  }
}
void* noteBig(void* p, size_t n, void* caller, const char* what) {
  if (p && g_bigAllocLimit && n >= g_bigAllocLimit) {
    liveAdd(p, n, caller);
    if (g_bigLog) n3dsLog("[mem] %s %u KB -> %p from %p\n", what, unsigned(n >> 10), p, caller);
  }
  return p;
}
}  // namespace
extern "C" {
void n3dsNoteCaller(void* p, void* caller) {
  if (!p || !g_bigAllocLimit) return;
  if (g_bigLog) n3dsLog("[mem] new-caller %p\n", caller);   // (Claude v86 diag) the line after a "[mem] malloc" from operator new names its caller
  Guard g;
  for (unsigned i = slotOf(p), k = 0; k < kSlots; k++, i = (i + 1) & (kSlots - 1)) {
    if (g_live[i].p == nullptr) return;
    if (g_live[i].p == p) { g_live[i].caller = caller; return; }
  }
}
void n3dsBigAllocReport(void) {
  if (!g_bigAllocLimit) return;
  struct Site { void* caller; size_t bytes; unsigned count; };
  static Site sites[512];
  unsigned ns = 0;
  size_t total = 0;
  {
    Guard g;
    for (const Live& l : g_live) {
      if (l.p == nullptr || l.p == (void*)1) continue;
      total += l.n;
      unsigned j = 0;
      while (j < ns && sites[j].caller != l.caller) j++;
      if (j == ns) { if (ns == 512) continue; sites[ns++] = Site{l.caller, 0, 0}; }
      sites[j].bytes += l.n; sites[j].count++;
    }
  }
  std::sort(sites, sites + ns, [](const Site& a, const Site& b) { return a.bytes > b.bytes; });
  n3dsLog("[mem] live tracked allocations: %u KB at %u call sites; largest:\n", unsigned(total >> 10), ns);
  for (unsigned j = 0; j < ns && j < 16; j++) n3dsLog("[mem]   %p %u KB in %u\n", sites[j].caller, unsigned(sites[j].bytes >> 10), sites[j].count);
}
// (Claude v74) contention on newlib's recursive locks (malloc above all): per thread, how often a lock was busy and how long it took to get it.
// The emulator runs every core on one host thread, so this only shows on the console.
}  // extern "C" (closed for the C++ below)
#include <sys/lock.h>
#include <atomic>
struct N3dsLockStat { std::atomic<unsigned> calls{0}, contended{0}; std::atomic<unsigned long long> waitTicks{0}; int core = -1; };
N3dsLockStat g_lockStats[12];
static std::atomic<int> g_lockSlots{0};
static thread_local int t_lockSlot = -1;
extern "C" {
void __real___libc_lock_acquire_recursive(_LOCK_RECURSIVE_T* l);
void __wrap___libc_lock_acquire_recursive(_LOCK_RECURSIVE_T* l) {
  if (t_lockSlot < 0) { const int s = g_lockSlots.fetch_add(1); t_lockSlot = s < 12 ? s : 11; if (s < 12) g_lockStats[s].core = svcGetProcessorID(); }
  N3dsLockStat& st = g_lockStats[t_lockSlot];
  st.calls.fetch_add(1, std::memory_order_relaxed);
  if (__libc_lock_try_acquire_recursive(l) == 0) return;   // libctru TryLock: 0 = taken
  const u64 t0 = svcGetSystemTick();
  __real___libc_lock_acquire_recursive(l);
  st.contended.fetch_add(1, std::memory_order_relaxed);
  st.waitTicks.fetch_add(svcGetSystemTick() - t0, std::memory_order_relaxed);
}
void n3dsLog(const char* fmt, ...);
void n3dsLockStatsLog(unsigned long frame) {   // and reset
  char line[480]; int at = std::snprintf(line, sizeof line, "[locks] frame %lu (thread core calls contended wait-ms):", frame);
  const int n = std::min(g_lockSlots.load(), 12);
  for (int i = 0; i < n && at < int(sizeof line) - 48; i++) {
    N3dsLockStat& st = g_lockStats[i];
    const unsigned c = st.calls.exchange(0), k = st.contended.exchange(0); const unsigned long long w = st.waitTicks.exchange(0);
    if (!c) continue;
    at += std::snprintf(line + at, sizeof line - at, " t%d c%d %u %u %.2f", i, st.core, c, k, double(w) * 1000.0 / SYSCLOCK_ARM11);
  }
  n3dsLog("%s\n", line);
}
}  // extern "C"
// (Claude v104 diag) N3DS_ALLOC_SITES=1: which call sites reach newlib's locked malloc/free (blocks over 1 KiB), per thread slot of [locks].
namespace asite {
struct Site { void* caller; unsigned count; unsigned long long bytes; };
constexpr int kSites = 48;
static Site g_sites[4][kSites];
static bool g_on = false;
static inline void note(void* caller, size_t n) {
  if (!g_on) return;
  const int s = t_lockSlot; if (s < 0 || s >= 4) return;
  Site* t = g_sites[s];
  for (int i = 0; i < kSites; i++) {
    if (t[i].caller == caller) { t[i].count++; t[i].bytes += n; return; }
    if (!t[i].caller) { t[i].caller = caller; t[i].count = 1; t[i].bytes = n; return; }
  }
}
}  // namespace asite
extern "C" void n3dsAllocSitesLog(unsigned long frame) {
  if (!asite::g_on) return;
  for (int s = 0; s < 4; s++) {
    asite::Site* t = asite::g_sites[s];
    std::sort(t, t + asite::kSites, [](const asite::Site& a, const asite::Site& b) { return a.count > b.count; });
    for (int i = 0; i < 12 && t[i].caller; i++)
      n3dsLog("[alloc-sites] frame %lu slot %d core %d %p calls %u bytes %llu\n", frame, s, g_lockStats[s].core, t[i].caller, t[i].count, t[i].bytes);
    std::memset(t, 0, sizeof asite::g_sites[s]);
  }
}
extern "C" void n3dsAllocSitesEnable() { asite::g_on = true; }
// (Claude v76) fast_alloc: small blocks (<= 1 KiB, ~85% of all allocations) from per-thread caches over 64 KiB size-class spans, so most malloc /
// free calls take no lock (newlib's malloc lock is shared by every core). Spans come from newlib (memalign) and stay with their class; at most
// kFaMaxSpans of them, after which small blocks come from newlib again. Pointers are told apart by a span table over the heap range. Off until
// gpu init reads performance.ini (fast_alloc): every earlier block is newlib's and stays so.
namespace fa {
constexpr size_t kSpan = 64u << 10;
constexpr int kClasses = 12;
static unsigned kFaMaxSpans = 64;                    // (Claude, Nov 15) set by n3dsFastAllocEnable from performance.ini fast_alloc_spans (was a fixed 160)
static const uint16_t kSize[kClasses] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024};   // (Claude v79: a 1408 B event class exhausted the span budget in the game: reverted)
static uint8_t kClassOf[65];                          // (n + 15) / 16 -> class
struct Block { Block* next; };
struct Central { LightLock lock; Block* head = nullptr; unsigned count = 0; uint8_t* bump = nullptr; uint8_t* end = nullptr; };
static Central g_central[kClasses];
static uint8_t* g_base = nullptr; static uint8_t* g_end = nullptr;
static uint8_t g_spanClass[2048];                     // (p - g_base) >> 16 -> class + 1 (0: not a span)
static std::atomic<unsigned> g_spans{0};
static std::atomic<bool> g_on{false};
std::atomic<unsigned long> g_fallbacks{0};
struct Cache { Block* head[kClasses] = {}; uint16_t n[kClasses] = {}; };
static thread_local Cache t_cache;
static inline int spanClassOf(const void* p) {
  const uint8_t* q = static_cast<const uint8_t*>(p);
  if (q < g_base || q >= g_end) return -1;
  return int(g_spanClass[size_t(q - g_base) >> 16]) - 1;
}
static bool refill(int c) {                           // central lock held by the caller: up to 32 blocks into this thread's cache
  Central& C = g_central[c]; Cache& t = t_cache;
  unsigned moved = 0;
  while (C.head && moved < 32) { Block* b = C.head; C.head = b->next; C.count--; b->next = t.head[c]; t.head[c] = b; moved++; }
  while (moved < 32) {
    if (C.bump + kSize[c] > C.end) {
      if (g_spans.load() >= kFaMaxSpans) break;
      uint8_t* s = static_cast<uint8_t*>(memalign(kSpan, kSpan));
      if (!s) break;
      if (s < g_base || s + kSpan > g_end) { free(s); break; }   // outside the table: give it back (cannot happen with the sbrk heap)
      g_spanClass[size_t(s - g_base) >> 16] = uint8_t(c + 1);
      g_spans.fetch_add(1);
      C.bump = s; C.end = s + kSpan;
    }
    Block* b = reinterpret_cast<Block*>(C.bump); C.bump += kSize[c];
    b->next = t.head[c]; t.head[c] = b; moved++;
  }
  t.n[c] = uint16_t(t.n[c] + moved);
  return moved > 0;
}
static inline void* alloc(size_t n) {
  if (!g_on.load(std::memory_order_relaxed) || n > 1024) return nullptr;
  const int c = kClassOf[(n + 15) >> 4];
  Cache& t = t_cache;
  if (!t.head[c]) {
    Central& C = g_central[c];
    LightLock_Lock(&C.lock); const bool ok = refill(c); LightLock_Unlock(&C.lock);
    if (!ok) { g_fallbacks.fetch_add(1, std::memory_order_relaxed); return nullptr; }
  }
  Block* b = t.head[c]; t.head[c] = b->next; t.n[c]--;
  return b;
}
static inline void release(void* p, int c) {
  Cache& t = t_cache;
  Block* b = static_cast<Block*>(p); b->next = t.head[c]; t.head[c] = b;
  if (++t.n[c] > 128) {                               // give 64 back to the class
    Block* first = t.head[c]; Block* last = first;
    for (int i = 1; i < 64; i++) last = last->next;
    t.head[c] = last->next; t.n[c] = uint16_t(t.n[c] - 64);
    Central& C = g_central[c];
    LightLock_Lock(&C.lock); last->next = C.head; C.head = first; C.count += 64; LightLock_Unlock(&C.lock);
  }
}
}  // namespace fa
extern "C" char* fake_heap_start; extern "C" char* fake_heap_end;
void n3dsFastAllocEnable(bool on, int maxSpans) {
  if (!on || fa::g_on.load()) return;
  if (maxSpans >= 32 && maxSpans <= 160) fa::kFaMaxSpans = unsigned(maxSpans);
  for (int i = 0, c = 0; i <= 64; i++) { while (c < fa::kClasses - 1 && size_t(i) * 16 > fa::kSize[c]) c++; fa::kClassOf[i] = uint8_t(c); }
  for (auto& C : fa::g_central) LightLock_Init(&C.lock);
  fa::g_base = reinterpret_cast<uint8_t*>(uintptr_t(fake_heap_start) & ~uintptr_t(fa::kSpan - 1));
  fa::g_end = fa::g_base + sizeof fa::g_spanClass * fa::kSpan;
  if (reinterpret_cast<uint8_t*>(fake_heap_end) > fa::g_end) { n3dsLog("[fast-alloc] heap larger than the span table: off\n"); return; }
  fa::g_on.store(true);
  n3dsLog("[fast-alloc] on: classes <= 1024 B, at most %u spans of 64 KB\n", fa::kFaMaxSpans);
}
void n3dsFastAllocLog() { n3dsLog("[fast-alloc] spans %u (%u KB), fallbacks %lu\n", fa::g_spans.load(), unsigned(fa::g_spans.load() * 64), fa::g_fallbacks.load()); }
extern "C" {
// (Claude v86) the renderer's block pools (uniform blocks, event nodes) keep freed blocks; when newlib cannot satisfy a request they are handed back first
void n3dsPoolsTrim(void);   // gpu_3ds.cpp
static void* retryAfterTrim(void* p, bool wanted, void* (*again)(void*), void* arg) { if (p || !wanted) return p; n3dsPoolsTrim(); return again(arg); }
void* __wrap_malloc(size_t n) { if (void* p = fa::alloc(n)) return p;
  asite::note(__builtin_return_address(0), n);
  void* p = retryAfterTrim(__real_malloc(n), n != 0, [](void* a) { return __real_malloc(*static_cast<size_t*>(a)); }, &n);
  return noteBig(p, n, __builtin_return_address(0), "malloc"); }
void* __wrap_calloc(size_t a, size_t b) { if (b && a <= 1024 / b) if (void* p = fa::alloc(a * b)) { std::memset(p, 0, a * b); return p; }
  size_t ab[2] = {a, b};
  void* p = retryAfterTrim(__real_calloc(a, b), a && b, [](void* x) { const size_t* v = static_cast<size_t*>(x); return __real_calloc(v[0], v[1]); }, ab);
  return noteBig(p, a * b, __builtin_return_address(0), "calloc"); }
void* __wrap_realloc(void* p, size_t n) {
  if (p) if (const int c = fa::spanClassOf(p); c >= 0) {   // a fast block: keep it when it fits, otherwise move it
    if (n && n <= fa::kSize[c]) return p;
    void* q = n ? __wrap_malloc(n) : nullptr;
    if (n && !q) return nullptr;
    if (q) std::memcpy(q, p, std::min<size_t>(n, fa::kSize[c]));
    fa::release(p, c);
    return q;
  }
  if (p && g_bigAllocLimit) liveDel(p);
  asite::note(__builtin_return_address(0), n);
  return noteBig(__real_realloc(p, n), n, __builtin_return_address(0), "realloc");
}
void* __wrap_aligned_alloc(size_t a, size_t n) { if (a <= 16) if (void* p = fa::alloc(n)) return p;   // fast blocks are 16-byte aligned
  size_t an[2] = {a, n};
  void* p = retryAfterTrim(__real_aligned_alloc(a, n), n != 0, [](void* x) { const size_t* v = static_cast<size_t*>(x); return __real_aligned_alloc(v[0], v[1]); }, an);
  return noteBig(p, n, __builtin_return_address(0), "aligned_alloc"); }
void __wrap_free(void* p) {
  if (p) if (const int c = fa::spanClassOf(p); c >= 0) { fa::release(p, c); return; }
  if (p && g_bigAllocLimit) liveDel(p);
  if (p) asite::note(__builtin_return_address(0), 0);
  __real_free(p);
}
}
