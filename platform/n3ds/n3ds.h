// The C interface between the game side of the 3DS build (clang: the game, Aurora's SDK, the PC layer) and the
// 3DS side (devkitARM g++: libctru, citro3d, the GX renderer). Plain C types only.
#pragma once
#include <stdint.h>

#define N3DS_ROOT "sdmc:/3ds/ww3ds"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t keys;            // libctru KEY_* bits held
  int16_t stickX, stickY;   // circle pad, about -156..156
  int16_t cstickX, cstickY; // C-stick (New 3DS)
} N3dsPad;

// libctru KEY_* bits (3ds/services/hid.h)
enum {
  N3DS_KEY_A = 1u << 0, N3DS_KEY_B = 1u << 1, N3DS_KEY_SELECT = 1u << 2, N3DS_KEY_START = 1u << 3,
  N3DS_KEY_DRIGHT = 1u << 4, N3DS_KEY_DLEFT = 1u << 5, N3DS_KEY_DUP = 1u << 6, N3DS_KEY_DDOWN = 1u << 7,
  N3DS_KEY_R = 1u << 8, N3DS_KEY_L = 1u << 9, N3DS_KEY_X = 1u << 10, N3DS_KEY_Y = 1u << 11,
  N3DS_KEY_ZL = 1u << 14, N3DS_KEY_ZR = 1u << 15, N3DS_KEY_TOUCH = 1u << 20,
};

int n3dsAudioInit(void);
int n3dsAudioOpen(int hz);
int n3dsAudioPut(const void* data, int bytes);
int n3dsAudioQueued(void);  // bytes, including the partial buffer
int n3dsAudioResume(void);
int n3dsAudioSetRate(int hz);

int n3dsPrepareExit(void);
void n3dsCloseLog(void);
void n3dsAudioShutdown(void);
int n3dsMemcardQuiesce(unsigned timeoutMs);
void n3dsSetLogPath(const char* path); // optional diagnostic log, set before n3dsInit
int n3dsInit(void);                 // screens, console, log; returns 1 on a New 3DS
void n3dsLog(const char* fmt, ...);
// Renderer owner-thread dispatch. Local functions are only called by the owner.
int n3dsRenderThreadStart(void);
int n3dsRenderThreadActive(void);
void n3dsRenderThreadJob(void (*job)(void*), void* context); // diagnostic job on the existing owner mailbox
uint32_t n3dsRenderStagingBudget(void); // configured once before recording, hard bounded
float n3dsCameraCullAspect(float aspect, float viewportWidth); // corrected full-width camera visibility
int n3dsRenderThreadInit(void);
uint32_t n3dsRenderThreadProcess(const uint8_t*, uint32_t, int*);
void n3dsRenderThreadFrame(void);
void n3dsRenderThreadRelease(void);
void n3dsGpuSync(void);
int n3dsRenderThreadBusy(void);   // (Claude) nonzero while the render owner runs a job (non-blocking)
void n3dsGpuShutdown(void);
int n3dsGpuInitLocal(void);
uint32_t n3dsGpuProcessLocal(const uint8_t*, uint32_t, int*);
void n3dsGpuEndFrameLocal(void);
void n3dsGpuShutdownLocal(void);

int n3dsGpuInit(void);              // citro3d and the GX renderer; 0 on failure
// One chunk of Aurora's GX stream; returns the bytes consumed (stops right after a draw-done command, *drawDone = 1)
uint32_t n3dsGpuProcess(const uint8_t* data, uint32_t size, int* drawDone);
void n3dsGpuEndFrame(void);         // the frame's XFB copy, then it is drawn and shown
int n3dsAptLoop(void);              // 0: the system asks the app to quit
void n3dsHidRead(N3dsPad* out);
uint64_t n3dsTicks(void);
void n3dsYield(void);
unsigned int n3dsCurrentThreadHandle(void);
void n3dsSetThreadPriority(unsigned int handle, int gcPriority);
int n3dsPushCreatePriority(int gcPriority);
void n3dsPopCreatePriority(int saved);   // 0: the calling thread; gcPriority -1: interrupt context               // sleeps 50 us so that other threads on the core can run
double n3dsTicksPerSecond(void);
void n3dsGameHeapReport(char* out, unsigned size);
void n3dsBigAllocReport(void);   // testing (N3DS_BIGALLOC_KB): the call sites holding the most live host memory (main_3ds.cpp)   // free / size of MEM1's root heap and the game heaps (pcstubs_3ds.cpp)

#ifdef __cplusplus
}
#endif
