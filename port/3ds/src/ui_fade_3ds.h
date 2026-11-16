#pragma once
#include <stdint.h>
#include <stddef.h>
namespace n3ds_ui {
// Fade native RGB565 artwork over black. Exact endpoints; bounded tables on the
// stack, no allocations, and no work on steady frames (caller caches opacity).
inline void fade565(uint16_t* dst,const uint16_t* src,size_t count,uint8_t alpha){
 uint16_t rb[32],g[64];
 for(unsigned i=0;i<32;++i)rb[i]=uint16_t((i*alpha+127)/255);
 for(unsigned i=0;i<64;++i)g[i]=uint16_t((i*alpha+127)/255);
 for(size_t i=0;i<count;++i){const unsigned c=src[i];
  dst[i]=uint16_t((rb[c>>11]<<11)|(g[(c>>5)&63]<<5)|rb[c&31]);
 }
}
// Any item identity change invalidates a held action, not only an empty slot.
inline uint32_t changedItems(uint32_t oldItems,uint32_t newItems){
 uint32_t changed=0;
 for(unsigned i=0;i<3;++i)if(((oldItems^newItems)>>(8*i))&255u)changed|=1u<<i;
 return changed;
}
}
