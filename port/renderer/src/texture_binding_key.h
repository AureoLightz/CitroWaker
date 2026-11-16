#pragma once
#include <cstdint>
namespace t2p {
// Texture identity and sampler state are a tuple, not a hash. In particular,
// forcing sentinel bits into a hash aliases CLAMP_TO_EDGE(0) and REPEAT(2).
struct TextureBindingKey {
 uint64_t generation=0;
 uint8_t wrapS=0,wrapT=0,magFilter=0,minFilter=0,mipFilter=0,minLevel=0,maxLevel=0;
 bool projective=false;uint16_t lodBias=0;
 bool operator==(const TextureBindingKey& b) const {
  return generation==b.generation && wrapS==b.wrapS && wrapT==b.wrapT && magFilter==b.magFilter && minFilter==b.minFilter &&
   mipFilter==b.mipFilter && minLevel==b.minLevel && maxLevel==b.maxLevel && projective==b.projective && lodBias==b.lodBias;
 }
};
template<class Unit> TextureBindingKey textureBindingKey(uint64_t generation,const Unit& u){
 return {generation,u.wrapS,u.wrapT,u.magFilter,u.minFilter,u.mipFilter,u.minLevel,u.maxLevel,u.projective,u.lodBias};
}
}
