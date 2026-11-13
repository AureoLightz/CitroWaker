// Texture conversion: GameCube texture formats -> PICA200 texture formats.
//   decodeGx*      what the GC's texture unit returns for every texel (8-bit RGBA, the TEV's view of the texture). Formulas follow the
//                  hardware behaviour as Dolphin / Aurora implement it (CMPR interpolates at 3/8 and 5/8; its transparent index is the
//                  midpoint colour with alpha 0).
//   picaDecode*    what the PICA texture unit returns (the model is Azahar's LookupTexture: Morton-tiled 8x8 tiles, ETC1 sub-tiles, ...).
//   chooseFormat   picks the smallest PICA format that reproduces the decoded GC texture exactly; if none is smaller than RGBA8 and lossy
//                  output is allowed, transcodes to RGB565 / RGBA4 / ETC1 / ETC1A4 and reports the error.
// Image rows keep their GX order (row 0 = t 0 = top). The PICA samples memory row 0 at v = 1 (hwcheck4 on Azahar: all 13 formats exact once the row order is
// reversed in the model), so the vertex shader outputs t' = q - t (VtxSetup::flipT) instead of the converter reordering rows.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace t2p {

// (Claude) texture-miss stage clock (ticks: svcGetSystemTick on the 3DS, ns elsewhere); only misses are timed.
// [0] mapTexture total [1] GX decode + fit [2] exact format pick (step 1) [3] lossy trial (step 2) [4] GPU upload [5] conversions [6] lossy accepted [7] longest mapTexture conversion (since reset)
extern uint64_t g_texProf[8];
uint64_t texProfTicks();

// ---- GameCube side
enum GxTexFmt : int { GXF_I4 = 0, GXF_I8 = 1, GXF_IA4 = 2, GXF_IA8 = 3, GXF_RGB565 = 4, GXF_RGB5A3 = 5, GXF_RGBA8 = 6, GXF_C4 = 8, GXF_C8 = 9, GXF_C14X2 = 10, GXF_CMPR = 14 };
enum GxTlutFmt : int { GXT_IA8 = 0, GXT_RGB565 = 1, GXT_RGB5A3 = 2 };

struct GxTlut {                       // resolved palette: entry i as 8-bit RGBA
  std::vector<uint8_t> rgba;          // 4 bytes per entry
};
GxTlut makeGxTlut(int tlutFmt, const uint8_t* bigEndianData, int entries);

size_t gxTextureBytes(int fmt, int w, int h);                // stored size of one mip level (width / height rounded up to the block size)
bool gxFormatHasPalette(int fmt);
// Decodes one mip level into w*h*4 bytes (top row first). Returns false (and sets err) for an unknown format, a short buffer or a missing palette.
bool decodeGxTexture(int fmt, int w, int h, const uint8_t* data, size_t size, const GxTlut* tlut, std::vector<uint8_t>& rgba, std::string& err);

// ---- PICA side (values are GPU_TEXCOLOR / TexturingRegs::TextureFormat numbers)
enum PicaTexFmt : int { PTF_RGBA8 = 0, PTF_RGB8 = 1, PTF_RGB5A1 = 2, PTF_RGB565 = 3, PTF_RGBA4 = 4, PTF_IA8 = 5, PTF_HILO8 = 6, PTF_I8 = 7, PTF_A8 = 8,
                        PTF_IA4 = 9, PTF_I4 = 10, PTF_A4 = 11, PTF_ETC1 = 12, PTF_ETC1A4 = 13 };
const char* picaTexFmtName(int fmt);
size_t picaTextureBytes(int fmt, int w, int h);              // w, h multiples of 8
// What the PICA samples at texel (x, y) of an image stored in `fmt` (Morton tiles, row-major tile order).
void picaSampleTexel(int fmt, const uint8_t* data, int w, int x, int y, uint8_t rgba[4]);
std::vector<uint8_t> picaDecodeImage(int fmt, const uint8_t* data, int w, int h);

// lossless encoders: return false if some texel is not representable exactly (data is still filled with the nearest representable value)
bool picaEncode(int fmt, const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& out, bool compareAlpha = true, bool verify = true);
// ETC1 (lossy). quality 0: base colour = quantised half-block average; 1: also tries (average - modifier) bases, which
// matters for flat colour regions because the modifier is added to all three channels (roughly 5x slower).
void etc1EncodeBlock(const uint8_t rgb[16][3], uint64_t& out, int quality);   // pixel index p = 4 * y + x
void picaEncodeEtc1(const std::vector<uint8_t>& rgba, int w, int h, bool withAlpha, int quality, std::vector<uint8_t>& out);

// ---- format choice
using TextureOutput = std::function<uint8_t*(int,int,int,int,const std::vector<size_t>&,size_t)>;
struct TexConvOptions {
  TextureOutput output; // optional final-format destination; nullptr falls back to owned data

  bool needAlpha = true;       // false: the TEV never reads this texture's alpha, so only the colour has to match
  bool allowLossy = true;      // may fall back to RGB565 / RGBA4 / ETC1 when the exact choice would be RGBA8 and the texture is large
  int lossyMinTexels = 1024;   // below this many texels RGBA8 is kept even if lossy would be smaller
  int etc1Quality = 1;
  bool fastLossyError = false; // exact error LUT and rejection shortcut; experimental
  bool tryEtc1 = true;         // false: ETC1 / ETC1A4 are not tried (encoding them is ~97% of a conversion's time: about a second per texture on the ARM11)
  double lossyMeanErrLimit = 2.0;   // a lossy format is accepted only if its mean and max error (8-bit units) are within these limits
  double lossyMaxErrLimit = 32.0;
};
struct PicaTexture {
  bool externalOutput=false;
  size_t encodedBytes=0;
  int fmt = PTF_RGBA8;
  int width = 0, height = 0;   // padded to a multiple of 8 (>= 8)
  std::vector<uint8_t> data;
  int levels = 1;                          // mip levels stored (level 0 first, each level Morton-tiled; PICA layout)
  std::vector<size_t> levelOffset;         // byte offset of every level inside `data`
  bool exact = true;
  double maxErr255 = 0, meanErr255 = 0;   // vs the GC texel values (colour; alpha too when needAlpha), 0 when exact
  std::string note;
};
// img: w*h*4 decoded GC texels. Pads to a multiple of 8 by repetition (exact for REPEAT wrapping of power-of-two sizes; flagged otherwise).
PicaTexture chooseFormat(const std::vector<uint8_t>& rgba, int w, int h, const TexConvOptions& = {});
// A mip chain: every level already a multiple of 8 in both dimensions (the PICA stops at 8x8). One format for all levels: the smallest one that is
// exact in every level, else a lossy one whose mean / max error over the whole chain is within the limits.
struct MipImage {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  // Large fitted images retain the compact decoded source instead of a second
  // full power-of-two RGBA image. These maps express exactly axisSource().
  int sourceWidth = 0;
  std::vector<int> sourceX, sourceY;
  const uint8_t* pixel(int x, int y) const {
    if (sourceX.empty()) return rgba.data() + (size_t(y) * w + x) * 4;
    return rgba.data() + (size_t(sourceY[y]) * sourceWidth + sourceX[x]) * 4;
  }
};
PicaTexture chooseFormatLevels(const std::vector<MipImage>& levels, const TexConvOptions& = {});

}  // namespace t2p
