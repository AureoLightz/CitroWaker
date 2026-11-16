// The few SDL3 functions the 3DS build keeps calling (Aurora's file IO, memory card files, the disc stream, the
// audio interface): file streams on stdio, paths on POSIX calls, audio through NDSP.
// SDL itself is not built for the 3DS.
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>

#include "n3ds.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

struct SDL_IOStream {
  FILE* f = nullptr;
  SDL_IOStatus status = SDL_IO_STATUS_READY;
};

namespace {
thread_local char tError[256] = "";
void setErr(const char* what) { std::snprintf(tError, sizeof tError, "%s: %s", what, std::strerror(errno)); }
}  // namespace

extern "C" {

const char* SDL_GetError(void) { return tError; }
bool SDL_SetError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(tError, sizeof tError, fmt, ap);
  va_end(ap);
  return false;
}
void SDL_free(void* mem) { std::free(mem); }
bool SDL_SetHint(const char*, const char*) { return true; }
bool SDL_InitSubSystem(SDL_InitFlags flags) {
  if (flags & ~SDL_INIT_AUDIO) return SDL_SetError("unsupported SDL subsystem on the 3DS");
  return !(flags & SDL_INIT_AUDIO) || n3dsAudioInit() || SDL_SetError("NDSP initialization failed (see log.txt)");
}
Uint32 SDL_rand_bits(void) {
  static Uint64 s = 0x9E3779B97F4A7C15ull;
  s ^= s << 13; s ^= s >> 7; s ^= s << 17;
  return Uint32(s >> 32);
}
bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority) { return true; }
const bool* SDL_GetKeyboardState(int* numkeys) {
  static bool keys[512] = {};
  if (numkeys) *numkeys = 512;
  return keys;
}
SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y) {
  if (x) *x = 0;
  if (y) *y = 0;
  return 0;
}
char* SDL_GetPrefPath(const char*, const char* app) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "sdmc:/3ds/%s/", app ? app : "app");
  return strdup(buf);
}

// ---- audio: push-based signed 16-bit stereo through libctru NDSP
// AI.cpp owns this single stream and calls it from its DMA thread.
struct SDL_AudioStream { int unused; };
static SDL_AudioStream audioStream;
static bool validAudioSpec(const SDL_AudioSpec* s) {
  return s && s->format == SDL_AUDIO_S16 && s->channels == 2 &&
         (s->freq == 32000 || s->freq == 48000);
}
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec* spec,
                                          SDL_AudioStreamCallback callback, void*) {
  if (device != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK || callback || !validAudioSpec(spec)) {
    SDL_SetError("3DS audio requires push-based S16 stereo at 32000 or 48000 Hz");
    return nullptr;
  }
  if (!n3dsAudioOpen(spec->freq)) {
    SDL_SetError("cannot open NDSP playback (see log.txt)");
    return nullptr;
  }
  return &audioStream;
}
bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* data, int bytes) {
  return s == &audioStream && n3dsAudioPut(data, bytes);
}
int SDL_GetAudioStreamQueued(SDL_AudioStream* s) {
  return s == &audioStream ? n3dsAudioQueued() : -1;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* s) {
  return s == &audioStream && n3dsAudioResume();
}
bool SDL_SetAudioStreamFormat(SDL_AudioStream* s, const SDL_AudioSpec* src, const SDL_AudioSpec* dst) {
  if (s != &audioStream || dst || !validAudioSpec(src)) return SDL_SetError("unsupported 3DS audio format");
  return n3dsAudioSetRate(src->freq);
}
const char* SDL_GetCurrentAudioDriver(void) { return "ndsp"; }

// ---- gamepads: none through SDL (pad_3ds.cpp reads the 3DS buttons)
SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc*) { return 0; }
bool SDL_SetJoystickVirtualButton(SDL_Joystick*, int, bool) { return false; }
bool SDL_SetJoystickVirtualAxis(SDL_Joystick*, int, Sint16) { return false; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad*) { return SDL_GAMEPAD_TYPE_UNKNOWN; }
bool SDL_GetGamepadButton(SDL_Gamepad*, SDL_GamepadButton) { return false; }
Sint16 SDL_GetGamepadAxis(SDL_Gamepad*, SDL_GamepadAxis) { return 0; }
SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID) { return nullptr; }
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID) { return nullptr; }
Uint16 SDL_GetJoystickVendorForID(SDL_JoystickID) { return 0; }
Uint16 SDL_GetJoystickProductForID(SDL_JoystickID) { return 0; }
int SDL_GetJoystickPlayerIndexForID(SDL_JoystickID) { return -1; }
SDL_JoystickID SDL_GetJoystickID(SDL_Joystick*) { return 0; }
SDL_Joystick* SDL_GetGamepadJoystick(SDL_Gamepad*) { return nullptr; }
SDL_GamepadBinding** SDL_GetGamepadBindings(SDL_Gamepad*, int* count) {
  if (count) *count = 0;
  return nullptr;
}

// ---- file streams
SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
  FILE* f = std::fopen(file, mode);
  if (!f) { setErr(file); return nullptr; }
  std::setvbuf(f, nullptr, _IOFBF, 64 * 1024);
  auto* s = new SDL_IOStream;
  s->f = f;
  return s;
}
bool SDL_CloseIO(SDL_IOStream* s) {
  if (!s) return false;
  const bool ok = std::fclose(s->f) == 0;
  delete s;
  return ok;
}
bool SDL_FlushIO(SDL_IOStream* s) { return s && std::fflush(s->f) == 0; }
Sint64 SDL_GetIOSize(SDL_IOStream* s) {
  if (!s) return -1;
  const long cur = std::ftell(s->f);
  if (std::fseek(s->f, 0, SEEK_END) != 0) return -1;
  const long end = std::ftell(s->f);
  std::fseek(s->f, cur, SEEK_SET);
  return end;
}
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* s) { return s ? s->status : SDL_IO_STATUS_ERROR; }
size_t SDL_ReadIO(SDL_IOStream* s, void* p, size_t n) {
  if (!s) return 0;
  const size_t r = std::fread(p, 1, n, s->f);
  if (r < n) s->status = std::ferror(s->f) ? SDL_IO_STATUS_ERROR : SDL_IO_STATUS_EOF;
  else s->status = SDL_IO_STATUS_READY;
  return r;
}
size_t SDL_WriteIO(SDL_IOStream* s, const void* p, size_t n) {
  if (!s) return 0;
  const size_t w = std::fwrite(p, 1, n, s->f);
  s->status = w < n ? SDL_IO_STATUS_ERROR : SDL_IO_STATUS_READY;
  return w;
}
Sint64 SDL_SeekIO(SDL_IOStream* s, Sint64 off, SDL_IOWhence whence) {
  if (!s) return -1;
  const int w = whence == SDL_IO_SEEK_SET ? SEEK_SET : whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END;
  if (std::fseek(s->f, long(off), w) != 0) { setErr("seek"); return -1; }
  s->status = SDL_IO_STATUS_READY;
  return std::ftell(s->f);
}
Sint64 SDL_TellIO(SDL_IOStream* s) { return s ? std::ftell(s->f) : -1; }
bool SDL_ReadU8(SDL_IOStream* s, Uint8* v) { return SDL_ReadIO(s, v, 1) == 1; }
bool SDL_ReadU16LE(SDL_IOStream* s, Uint16* v) { return SDL_ReadIO(s, v, 2) == 2; }   // ARM: little endian
bool SDL_ReadU32LE(SDL_IOStream* s, Uint32* v) { return SDL_ReadIO(s, v, 4) == 4; }
bool SDL_WriteU8(SDL_IOStream* s, Uint8 v) { return SDL_WriteIO(s, &v, 1) == 1; }
bool SDL_WriteU16LE(SDL_IOStream* s, Uint16 v) { return SDL_WriteIO(s, &v, 2) == 2; }
bool SDL_WriteU32LE(SDL_IOStream* s, Uint32 v) { return SDL_WriteIO(s, &v, 4) == 4; }
bool SDL_WriteS32LE(SDL_IOStream* s, Sint32 v) { return SDL_WriteIO(s, &v, 4) == 4; }

// ---- paths
bool SDL_CreateDirectory(const char* path) {
  // creates parents too, as SDL does
  char buf[512];
  std::snprintf(buf, sizeof buf, "%s", path);
  for (char* p = buf + 1; *p; p++) {
    if (*p == '/' && p[-1] != ':') { *p = 0; mkdir(buf, 0777); *p = '/'; }
  }
  if (mkdir(buf, 0777) != 0 && errno != EEXIST) { setErr(path); return false; }
  return true;
}
bool SDL_GetPathInfo(const char* path, SDL_PathInfo* info) {
  struct stat st;
  if (stat(path, &st) != 0) {
    if (info) std::memset(info, 0, sizeof *info);
    setErr(path);
    return false;
  }
  if (info) {
    std::memset(info, 0, sizeof *info);
    info->type = S_ISDIR(st.st_mode) ? SDL_PATHTYPE_DIRECTORY : S_ISREG(st.st_mode) ? SDL_PATHTYPE_FILE : SDL_PATHTYPE_OTHER;
    info->size = Uint64(st.st_size);
  }
  return true;
}
bool SDL_RemovePath(const char* path) {
  if (std::remove(path) == 0 || rmdir(path) == 0) return true;
  if (errno == ENOENT) return true;
  setErr(path);
  return false;
}
bool SDL_RenamePath(const char* oldpath, const char* newpath) {
  std::remove(newpath);   // FAT: rename does not replace
  if (std::rename(oldpath, newpath) != 0) { setErr(oldpath); return false; }
  return true;
}

}  // extern "C"

// (Claude v111) the PGO-instrumented build keeps every function alive (profile data points at them), including mouse paths the
// normal build drops with --gc-sections. Weak stubs: never called on the 3DS, and a real definition elsewhere wins.
extern "C" __attribute__((weak)) SDL_MouseButtonFlags SDL_GetGlobalMouseState(float* x, float* y) { if (x) *x = 0; if (y) *y = 0; return 0; }
extern "C" __attribute__((weak)) SDL_MouseButtonFlags SDL_GetRelativeMouseState(float* x, float* y) { if (x) *x = 0; if (y) *y = 0; return 0; }
namespace aurora::input { __attribute__((weak)) void get_mouse_scroll(float* x, float* y) noexcept { if (x) *x = 0; if (y) *y = 0; } }
