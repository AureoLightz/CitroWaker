// NDSP owns submitted buffers until DONE. Keep samples in linear memory and
// flush the ARM data cache before handing them to the DSP.
extern "C" { unsigned g_n3dsAudioBlocks = 0; }   // (Claude) blocks queued to NDSP, for the watchdog log
#include <3ds.h>
#include <cstdlib>
#include <cstring>
#include "n3ds.h"

namespace {
constexpr int kBuffers = 24;
constexpr int kFrames = 512;
constexpr int kBytes = kFrames * 4;
LightLock mutex;
bool initialized = false, opened = false;
int rate = 0, partial = 0;
u8* samples = nullptr;
u8 staging[kBytes];
ndspWaveBuf waves[kBuffers];
struct Guard {
  Guard() { LightLock_Lock(&mutex); }
  ~Guard() { LightLock_Unlock(&mutex); }
};
bool busy(const ndspWaveBuf& w) {
  return w.status == NDSP_WBUF_QUEUED || w.status == NDSP_WBUF_PLAYING;
}
void shutdown() {
  Guard guard;
  if (!initialized) return;
  opened = false;
  ndspChnWaveBufClear(0);
  ndspExit(); // stop the DSP worker before freeing its buffers
  linearFree(samples);
  samples = nullptr;
  initialized = false;
}
}
extern "C" void n3dsAudioShutdown(void){shutdown();}
extern "C" int n3dsAudioInit(void) {
  Guard guard;
  if (initialized) return 1;
  Result rc = ndspInit();
  if (R_FAILED(rc)) {
    n3dsLog("[audio] ndspInit failed: 0x%08lx; check sdmc:/3ds/dspfirm.cdc\n", (unsigned long)rc);
    return 0;
  }
  samples = static_cast<u8*>(linearAlloc(kBuffers * kBytes));
  if (!samples) { ndspExit(); n3dsLog("[audio] no linear memory for NDSP\n"); return 0; }
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  initialized = true;
  std::atexit(shutdown);
  return 1;
}
extern "C" int n3dsAudioOpen(int hz) {
  if (hz != 32000 && hz != 48000) return 0;
  if (!n3dsAudioInit()) return 0;
  Guard guard;
  if (opened) return 0;
  ndspChnReset(0);
  ndspChnSetPaused(0, true);
  ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
  ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
  ndspChnSetRate(0, float(hz));
  float mix[12] = {1.0f, 1.0f};
  ndspChnSetMix(0, mix);
  std::memset(waves, 0, sizeof waves);
  partial = 0;
  rate = hz;
  opened = true;
  n3dsLog("[audio] NDSP S16 stereo %d Hz, %d x %d frames\n", hz, kBuffers, kFrames);
  return 1;
}
extern "C" int n3dsAudioResume(void) {
  Guard guard;
  if (!opened) return 0;
  ndspChnSetPaused(0, false);
  return 1;
}
extern "C" int n3dsAudioSetRate(int hz) {
  Guard guard;
  if (!opened || (hz != 32000 && hz != 48000)) return 0;
  if (rate != hz) {
    ndspChnWaveBufClear(0);
    std::memset(waves, 0, sizeof waves);
    partial = 0;
    ndspChnSetRate(0, float(hz));
    rate = hz;
  }
  return 1;
}
extern "C" int n3dsAudioQueued(void) {
  Guard guard;
  if (!opened) return -1;
  int bytes = partial;
  for (const auto& w : waves) if (busy(w)) bytes += kBytes;
  return bytes;
}
extern "C" int n3dsAudioPut(const void* data, int bytes) {
  Guard guard;
  if (!opened || bytes < 0 || bytes % 4 || (!data && bytes)) return 0;
  int available = 0;
  for (const auto& w : waves) if (!busy(w)) available += kBytes;
  // Reject the entire push if it cannot fit; never overwrite an active buffer.
  if (bytes > available + kBytes - 4 - partial) return 0;
  auto* src = static_cast<const u8*>(data);
  while (bytes) {
    int take = bytes < kBytes - partial ? bytes : kBytes - partial;
    std::memcpy(staging + partial, src, take);
    partial += take; src += take; bytes -= take;
    if (partial != kBytes) continue;
    for (int i = 0; i < kBuffers; ++i) {
      auto& w = waves[i];
      if (busy(w)) continue;
      std::memcpy(samples + i * kBytes, staging, kBytes);
      DSP_FlushDataCache(samples + i * kBytes, kBytes);
      std::memset(&w, 0, sizeof w);
      w.data_vaddr = samples + i * kBytes;
      w.nsamples = kFrames; // stereo frames, not individual channel samples
      ndspChnWaveBufAdd(0, &w);
      g_n3dsAudioBlocks++;
      partial = 0;
      break;
    }
  }
  return 1;
}
