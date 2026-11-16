// GX texture binding -> PICA texture + sampler.
// The PICA wants power-of-two sizes (citro3d refuses everything else; whether the GPU itself would cope is untested on hardware), 8..1024 per side, and its
// mip chain stops at 8x8. GX textures are any size that is a multiple of their block, with any wrap mode. mapTexture() fits the image per axis:
//   power of two >= 8                    unchanged, same wrap mode
//   smaller than 8 (1, 2, 4)             clamp: edge-padded to 8.  repeat: tiled to 8.  mirror: mirrored copies to 8 and the PICA sampler uses REPEAT (a
//                                        mirrored period [img, flip] repeated is exactly GX's mirror).  Coordinates are scaled by n/8 in all three cases,
//                                        because GX coordinate 1 is one image and the PICA's is eight texels
//   anything else (24, 40, 96, 320, ...) clamp: edge-padded to the next power of two, coordinates scaled by n/D (exact for nearest and for bilinear,
//                                        because the padding replicates the edge texel, which is what the clamp does).
//                                        repeat / mirror: no exact mapping exists; the image is stretched to the power of two (nearest) and the result is flagged
// The per-axis coordinate scale (scaleS, scaleT) belongs in the vertex shader: s' = s * scaleS, t' = q - t * scaleT (the flip from texconv.h included).
#pragma once
#include <string>
#include <vector>

#include "gxstate.h"
#include "texconv.h"
#include "texfit.h"

namespace t2p {

struct PicaSamplerDesc {
  uint8_t wrapS = 0, wrapT = 0;       // 0 clamp, 1 repeat, 2 mirror (map to GPU_CLAMP_TO_EDGE / GPU_REPEAT / GPU_MIRRORED_REPEAT)
  bool magLinear = false, minLinear = false;
  uint8_t mipFilter = 0;              // 0 no mip levels, 1 nearest between levels, 2 linear between levels
  int minLevel = 0, maxLevel = 0;     // LOD clamp, in levels of the stored chain
  float lodBias = 0;
};

struct MappedTexture {
  bool ok = false;
  std::string error;
  int fmt = PTF_RGBA8;
  int width = 0, height = 0;          // PICA sizes (power of two)
  int levels = 1;
  std::vector<uint8_t> data;          // levels back to back, level 0 first
  size_t encodedBytes=0;
  bool externalOutput=false;
  std::vector<size_t> levelOffset;
  float scaleS = 1, scaleT = 1;       // image coordinate -> texture coordinate scale (1 when the image fills the texture)
  PicaSamplerDesc sampler;
  bool exact = true;                  // the texels a GX draw would fetch are the ones the PICA fetches (colour and, when needed, alpha)
  double maxErr255 = 0, meanErr255 = 0;
  std::vector<std::string> notes;     // every deviation from the GX result
  std::vector<std::string> info;      // things that are modelled but unverified (filtering, LOD)
};

struct TexMapOptions {
  bool needAlpha = true;              // overrides conv.needAlpha
  TexConvOptions conv;
};

// The sampler state of a binding whose image was mapped to `levels` levels (filters, LOD clamp and bias differ per binding even when the image is shared).
PicaSamplerDesc samplerFor(const GxTexBinding& b, int levels);
// Stored size of all mip levels a binding uses (what mapTexture() wants as imageSize).
size_t gxImageBytes(const GxTexBinding& b);

// image: the GX image data of this binding (all mip levels back to back, as stored in memory).
MappedTexture mapTexture(const GxTexBinding& b, const uint8_t* image, size_t imageSize, const GxTlut* tlut, const TexMapOptions& opt = {});

}  // namespace t2p
