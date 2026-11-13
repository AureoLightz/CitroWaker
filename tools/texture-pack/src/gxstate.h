// Converter-only binding type extracted unchanged from the project renderer.
#pragma once
#include <cstdint>
namespace t2p {
struct GxTexBinding {
  bool valid = false;           // the image registers of this unit were written
  uint32_t addr = 0;            // physical main-memory address of the image
  int width = 0, height = 0;
  int fmt = 0;                  // GxTexFmt (texconv.h); C4 / C8 / C14X2 use the palette selected by tlutOffset
  uint8_t wrapS = 0, wrapT = 0; // GX_CLAMP 0, GX_REPEAT 1, GX_MIRROR 2
  bool magLinear = false, minLinear = false;
  uint8_t mipMode = 0;          // 0 none, 1 nearest between levels, 2 linear between levels
  float lodBias = 0, minLod = 0, maxLod = 0;
  bool edgeLod = false, biasClamp = false;
  uint8_t aniso = 0;            // log2 of the anisotropy
  uint32_t tlutOffset = 0;      // TX_SETTLUT: TMEM offset of the palette (same units as GxTlutLoad::tmemOffset)
  int tlutFmt = 0;              // GxTlutFmt
  int mipLevels() const;        // levels stored behind `addr` (1 without mipmapping)
};

}
