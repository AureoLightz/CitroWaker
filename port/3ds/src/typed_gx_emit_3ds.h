#pragma once
// Included AFTER the real __gx.h in the generated 3DS SDK header.
#include "typed_gx_api_3ds.h"
#include <cstring>
namespace n3ds_typed {
inline bool allowed() {
  return g_n3dsTypedFrame && !g_n3dsTypedPrimitive &&
         !aurora::gx::fifo::detail::sInDisplayList &&
         !aurora::gx::fifo::detail::sCaptureHooks;
}
inline void barrier() {
  // Frame-active inline publish completes the whole older stream. No reset:
  // GX_AUTO offsets remain meaningful, and already-published bytes cost no parse.
  if (aurora::gx::fifo::detail::sBufferSize != g_n3dsTypedConsumedSize)
    aurora::gx::fifo::publish();
}
inline void bp(uint32_t v) {
  const unsigned id=v>>24;
  if(allowed() && id!=0x45 && id!=0x52 && id!=0x65) {
    barrier(); n3dsTypedApplyBp(v); return;
  }
  aurora::gx::fifo::write_u8(0x61); aurora::gx::fifo::write_u32(v);
}
inline void cp(uint8_t op,uint8_t addr,uint32_t v) {
  if(op==8 && allowed()){barrier();n3dsTypedApplyCp(addr,v);return;}
  aurora::gx::fifo::write_u8(op);aurora::gx::fifo::write_u8(addr);aurora::gx::fifo::write_u32(v);
}
inline bool xfWords(uint32_t addr,const uint32_t* words,uint32_t n) {
  if(!allowed() || n>16)return false;
  uint32_t be[16];
  for(uint32_t i=0;i<n;++i)be[i]=::bswap(words[i]);
  barrier();n3dsTypedApplyXf(addr,reinterpret_cast<const uint8_t*>(be),n);return true;
}
inline bool xfRows(uint32_t addr,const float* m,uint32_t rows,uint32_t cols,uint32_t stride) {
  if(!allowed() || rows*cols>16)return false;
  uint32_t bits[16];
  for(uint32_t r=0;r<rows;++r)for(uint32_t c=0;c<cols;++c)
    std::memcpy(&bits[r*cols+c],m+r*stride+c,4);
  return xfWords(addr,bits,rows*cols);
}
inline void xfReg(uint32_t addr,uint32_t v) {
  if(xfWords(addr,&v,1))return;
  aurora::gx::fifo::write_u8(0x10);aurora::gx::fifo::write_u32(addr);aurora::gx::fifo::write_u32(v);
}
}
#undef GX_WRITE_RAS_REG
#define GX_WRITE_RAS_REG(value) n3ds_typed::bp(static_cast<uint32_t>(value))
#undef GX_WRITE_XF_REG
#define GX_WRITE_XF_REG(addr,value) n3ds_typed::xfReg(0x1000u+(addr),static_cast<uint32_t>(value))
#undef GX_WRITE_CP_REG
#define GX_WRITE_CP_REG(addr,value) n3ds_typed::cp(8,static_cast<uint8_t>(addr),static_cast<uint32_t>(value))
#undef GX_WRITE_SOME_REG2
#undef GX_WRITE_SOME_REG3
#undef GX_WRITE_SOME_REG4
#define GX_WRITE_SOME_REG2(a,b,c,addr) n3ds_typed::cp(static_cast<uint8_t>(a),static_cast<uint8_t>(b),static_cast<uint32_t>(c))
#define GX_WRITE_SOME_REG3(a,b,c,addr) GX_WRITE_SOME_REG2(a,b,c,addr)
#define GX_WRITE_SOME_REG4(a,b,c,addr) GX_WRITE_SOME_REG2(a,b,c,addr)
