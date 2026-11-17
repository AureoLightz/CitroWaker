// (Claude v138) Inline fast paths for the GX FIFO writers the game calls per vertex / per register (GXCmd1u32, GXPosition3f32,
// ...). Force-included (-include) into game units by build.sh, never into Aurora's own GX sources (they define the out-of-line
// functions, which stay the slow path: display-list recording and buffer growth). Same bytes in the same order as Aurora's
// fifo::write_* (big-endian values appended to detail::sBufferData), so the command stream is unchanged.
#pragma once
#if defined(__3DS__) && defined(TARGET_PC)
#include <dolphin/gx/GXVert.h>
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t* n3ds_gxf_data __asm__("_ZN6aurora2gx4fifo6detail11sBufferDataE");
extern uint32_t n3ds_gxf_size __asm__("_ZN6aurora2gx4fifo6detail11sBufferSizeE");
extern uint32_t n3ds_gxf_cap __asm__("_ZN6aurora2gx4fifo6detail15sBufferCapacityE");
#ifdef __cplusplus
extern bool n3ds_gxf_dl __asm__("_ZN6aurora2gx4fifo6detail14sInDisplayListE");
#else
extern _Bool n3ds_gxf_dl __asm__("_ZN6aurora2gx4fifo6detail14sInDisplayListE");
#endif
#ifdef __cplusplus
}
#endif

// room for n more bytes in the main FIFO buffer (not recording a display list): then the caller writes them directly
static inline __attribute__((always_inline)) uint8_t* n3ds_gxf_room(uint32_t n) {
  if (__builtin_expect(!n3ds_gxf_dl && n3ds_gxf_cap - n3ds_gxf_size >= n, 1)) {
    uint8_t* p = n3ds_gxf_data + n3ds_gxf_size;
    n3ds_gxf_size += n;
    return p;
  }
  return 0;
}
static inline __attribute__((always_inline)) void n3ds_gxf_be32(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }
static inline __attribute__((always_inline)) void n3ds_gxf_be16(uint8_t* p, uint16_t v) { v = __builtin_bswap16(v); memcpy(p, &v, 2); }
static inline __attribute__((always_inline)) uint32_t n3ds_gxf_f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static inline void n3ds_GXCmd1u8(u8 x) { uint8_t* p = n3ds_gxf_room(1); if (p) *p = x; else GXCmd1u8(x); }
static inline void n3ds_GXCmd1u16(u16 x) { uint8_t* p = n3ds_gxf_room(2); if (p) n3ds_gxf_be16(p, x); else GXCmd1u16(x); }
static inline void n3ds_GXCmd1u32(u32 x) { uint8_t* p = n3ds_gxf_room(4); if (p) n3ds_gxf_be32(p, x); else GXCmd1u32(x); }
static inline void n3ds_GXParam1u8(u8 x) { uint8_t* p = n3ds_gxf_room(1); if (p) *p = x; else GXParam1u8(x); }
static inline void n3ds_GXParam1u16(u16 x) { uint8_t* p = n3ds_gxf_room(2); if (p) n3ds_gxf_be16(p, x); else GXParam1u16(x); }
static inline void n3ds_GXParam1u32(u32 x) { uint8_t* p = n3ds_gxf_room(4); if (p) n3ds_gxf_be32(p, x); else GXParam1u32(x); }
static inline void n3ds_GXPosition3f32(f32 x, f32 y, f32 z) {
  uint8_t* p = n3ds_gxf_room(12);
  if (p) { n3ds_gxf_be32(p, n3ds_gxf_f(x)); n3ds_gxf_be32(p + 4, n3ds_gxf_f(y)); n3ds_gxf_be32(p + 8, n3ds_gxf_f(z)); }
  else GXPosition3f32(x, y, z);
}
static inline void n3ds_GXPosition2f32(f32 x, f32 y) {
  uint8_t* p = n3ds_gxf_room(8);
  if (p) { n3ds_gxf_be32(p, n3ds_gxf_f(x)); n3ds_gxf_be32(p + 4, n3ds_gxf_f(y)); } else GXPosition2f32(x, y);
}
static inline void n3ds_GXPosition3s16(s16 x, s16 y, s16 z) {
  uint8_t* p = n3ds_gxf_room(6);
  if (p) { n3ds_gxf_be16(p, (u16)x); n3ds_gxf_be16(p + 2, (u16)y); n3ds_gxf_be16(p + 4, (u16)z); } else GXPosition3s16(x, y, z);
}
static inline void n3ds_GXNormal3f32(f32 x, f32 y, f32 z) {
  uint8_t* p = n3ds_gxf_room(12);
  if (p) { n3ds_gxf_be32(p, n3ds_gxf_f(x)); n3ds_gxf_be32(p + 4, n3ds_gxf_f(y)); n3ds_gxf_be32(p + 8, n3ds_gxf_f(z)); }
  else GXNormal3f32(x, y, z);
}
static inline void n3ds_GXColor4u8(u8 r, u8 g, u8 b, u8 a) {
  uint8_t* p = n3ds_gxf_room(4);
  if (p) { p[0] = r; p[1] = g; p[2] = b; p[3] = a; } else GXColor4u8(r, g, b, a);
}
static inline void n3ds_GXColor1u32(u32 c) { uint8_t* p = n3ds_gxf_room(4); if (p) n3ds_gxf_be32(p, c); else GXColor1u32(c); }
static inline void n3ds_GXTexCoord2f32(f32 s, f32 t) {
  uint8_t* p = n3ds_gxf_room(8);
  if (p) { n3ds_gxf_be32(p, n3ds_gxf_f(s)); n3ds_gxf_be32(p + 4, n3ds_gxf_f(t)); } else GXTexCoord2f32(s, t);
}
static inline void n3ds_GXTexCoord2s16(s16 s, s16 t) {
  uint8_t* p = n3ds_gxf_room(4);
  if (p) { n3ds_gxf_be16(p, (u16)s); n3ds_gxf_be16(p + 2, (u16)t); } else GXTexCoord2s16(s, t);
}
#define GXCmd1u8(x) n3ds_GXCmd1u8(x)
#define GXCmd1u16(x) n3ds_GXCmd1u16(x)
#define GXCmd1u32(x) n3ds_GXCmd1u32(x)
#define GXParam1u8(x) n3ds_GXParam1u8(x)
#define GXParam1u16(x) n3ds_GXParam1u16(x)
#define GXParam1u32(x) n3ds_GXParam1u32(x)
#define GXPosition3f32(x, y, z) n3ds_GXPosition3f32(x, y, z)
#define GXPosition2f32(x, y) n3ds_GXPosition2f32(x, y)
#define GXPosition3s16(x, y, z) n3ds_GXPosition3s16(x, y, z)
#define GXNormal3f32(x, y, z) n3ds_GXNormal3f32(x, y, z)
#define GXColor4u8(r, g, b, a) n3ds_GXColor4u8(r, g, b, a)
#define GXColor1u32(c) n3ds_GXColor1u32(c)
#define GXTexCoord2f32(s, t) n3ds_GXTexCoord2f32(s, t)
#define GXTexCoord2s16(s, t) n3ds_GXTexCoord2s16(s, t)
#endif
