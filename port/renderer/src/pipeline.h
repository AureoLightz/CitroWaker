// Pipeline state: GX alpha compare / blend / z mode / fog -> PICA200 register values.
// Everything here is game-agnostic. Output values use the libctru / citro3d enum numbering so they can be passed to
// C3D_AlphaTest / C3D_AlphaBlend / C3D_DepthTest / C3D_FogLut* directly. PICA-side semantics follow Azahar's shader
// generator (verified by hwcheck2 on the Azahar GPU path; not yet on the real console).
#pragma once
#include <cstdint>
#include <string>

namespace t2p {

// ---- test functions: libctru GPU_TESTFUNC
enum : uint8_t { PF_NEVER = 0, PF_ALWAYS = 1, PF_EQUAL = 2, PF_NOTEQUAL = 3, PF_LESS = 4, PF_LEQUAL = 5, PF_GREATER = 6, PF_GEQUAL = 7 };
uint8_t picaFuncFromGx(int gxCompare);  // GX_NEVER 0, LESS 1, EQUAL 2, LEQUAL 3, GREATER 4, NEQUAL 5, GEQUAL 6, ALWAYS 7

// ---- alpha test. GX has two compares joined by AND/OR/XOR/XNOR; PICA has one. The GX pair is turned into the set of
// 8-bit alpha values that pass ("truth set") and that set is matched against every single PICA compare.
struct GxAlphaCompare { uint8_t comp0 = 7, ref0 = 0, op = 1, comp1 = 7, ref1 = 0; };  // op: GX_AOP_AND 0, OR 1, XOR 2, XNOR 3
struct PicaAlphaTest {
  bool enable = false;     // false = always pass
  uint8_t func = PF_ALWAYS, ref = 0;
  bool exact = true;       // false: no single PICA compare reproduces the GX test
  int mismatches = 0;      // number of the 256 alpha values classified differently (0 when exact)
  std::string note;
};
bool gxAlphaPasses(const GxAlphaCompare&, int alpha);
PicaAlphaTest alphaFromGx(const GxAlphaCompare&);

// ---- blend / logic op
struct GxBlend { uint8_t mode = 0, src = 1, dst = 0, logic = 3; };  // mode: NONE 0, BLEND 1, LOGIC 2, SUBTRACT 3
// libctru GPU_BLENDFACTOR: ZERO 0, ONE 1, SRC_COLOR 2, 1-SRC_COLOR 3, DST_COLOR 4, 1-DST_COLOR 5, SRC_ALPHA 6, 1-SRC_ALPHA 7,
// DST_ALPHA 8, 1-DST_ALPHA 9.  GPU_BLENDEQUATION: ADD 0, SUBTRACT 1, REVERSE_SUBTRACT 2, MIN 3, MAX 4.
// GPU_LOGICOP: CLEAR 0, AND 1, AND_REVERSE 2, COPY 3, SET 4, COPY_INVERTED 5, NOOP 6, INVERT 7, NAND 8, OR 9, NOR 10,
// XOR 11, EQUIV 12, AND_INVERTED 13, OR_REVERSE 14, OR_INVERTED 15.
struct PicaBlend {
  bool useLogicOp = false;
  uint8_t logicOp = 3;
  uint8_t eqRgb = 0, eqAlpha = 0;
  uint8_t srcRgb = 1, dstRgb = 0, srcAlpha = 1, dstAlpha = 0;
  bool blendsAnything = false;   // false = plain overwrite (ONE, ZERO)
  bool needsDstAlpha = false;    // a factor reads destination alpha (needs an RGBA target; GC RGB8 targets read 1.0)
  bool exact = true;
  std::string note;
};
PicaBlend blendFromGx(const GxBlend&);

// ---- depth test
struct GxZMode { bool compare = true; uint8_t func = 3; bool update = true; };
struct PicaDepth {
  bool testEnable = true;  // PICA only writes depth while the test is enabled; a GX "compare off, update on" becomes ALWAYS
  uint8_t func = PF_LEQUAL;
  bool write = true;
};
PicaDepth depthFromGx(const GxZMode&);

// ---- depth buffer value mapping (C3D_DepthMap). GX clip space has z/w = -1 at the near plane and 0 at the far plane
// (MTXFrustum/MTXOrtho: z_clip = -n/(f-n)*z_eye - f*n/(f-n)); libctru's default map (true, -1, 0) assumes the opposite
// (near 0, far -1). Scale +1 / offset +1 gives depth = z/w + 1 in [0,1] with near = 0, far = 1, the console's convention, so
// a 0xFFFFFF clear and LEQUAL/LESS/... tests order fragments exactly as on the GC. PICA zBuffer=false is the W-buffer used
// for fog (see PicaFog): depth = z_clip*scale + w_clip*offset, clamped to [0,1].
// Both maps cannot coexist inside one frame (depth values would not be comparable). Policy for the integration: a frame whose
// draws use fog renders all its draws with the W-buffer map of the fog range D (non-fogged draws included, fog unit off);
// a frame without fog uses the z map. Geometry beyond D shares depth 1.0 under the W map, which is the only ordering loss.
// (Claude v84) The console's depth unit wraps a fragment depth >= 1.0 to 0 (it does not clamp as Azahar does): the v83 log read 0x000000 where the
// emulator has 0xFFFFE3, and far geometry (horizon ocean, distant cliffs) was drawn over nearer geometry. Every map therefore ends at kPicaDepthMax < 1:
// geometry spans [0, kPicaDepthMax] (ordering unchanged: a positive scale), copy-clears to the far value write kPicaDepthClearFar (above any geometry
// with the console's ~2^-12 relative W error) and frame clears keep 0xFFFFFF.
constexpr float kPicaDepthMax = 255.0f / 256.0f;
constexpr float kPicaDepthClearFar = 1023.0f / 1024.0f;
// (Claude v90) depth fold. The console's PICA ignores GPUREG_DEPTHMAP_SCALE / OFFSET (0x4D / 0x4E): v88/v89 boot probes read the registers back
// holding the written values, yet every stored depth was the citro3d default map (scale -1, offset 0): Z mode -z/w, W mode -z. Azahar applies them.
// With the fold on, a map (s, o) is moved into the vertex output instead: z' = -(s z + o w), so the default map stores -z'/w = s z/w + o (Z mode) or
// -z' = s z + o w (W mode), the same values on both, and every draw leaves the registers at (-1, 0). Clipping then follows the stored depth
// (0 <= depth <= 1 in Z mode, 0 <= depth <= w in W mode) instead of the GX near/far planes.
extern bool g_picaDepthFold;
// (Claude v93) EXPERIMENT (perf depth_z=1): every draw uses the Z map and fog is off. The console's W-buffer mode does not store what Azahar does (v92 dock
// capture: ~1/100 of the emulator's values, ~170 levels for the whole scene), the Z map matched every probe. Tells whether the overlap is the W-buffer alone.
extern bool g_picaForceZDepth;
// (Claude v107) reversed Z (perf depth_reverse, default 1; only with depth_z + fold). The vertex shader outputs float24 (16-bit mantissa): the forward
// map stores 1 - n/d, which crowds the whole scene into the top ~2^-7 of the range (console v106 captures: 0xFD0000..0xFFC000) where float24 has
// ~2^-17 steps -> distant roofs/cliffs/sea z-fight. Reversed, the stored value is kPicaDepthMax*(1-g) (g = GX depth in [0,1]): near = kPicaDepthMax,
// far = 0, n/d near 0 keeps full float precision. Depth tests are mirrored (LESS<->GREATER), the frame clear is 0, far copy-clears write 0.
extern bool g_picaDepthReverse;
inline bool picaReverseZ() { return g_picaDepthReverse && g_picaForceZDepth && g_picaDepthFold; }
uint8_t picaMirrorDepthFunc(uint8_t f);
inline uint32_t picaFrameClearDepth() { return picaReverseZ() ? 0u : 0x00FFFFFFu; }
// GX depth g in [0,1] (fraction of the viewport range) -> the value stored for a constant-depth write
inline float picaConstDepth(double g, bool farClear) {
  if (picaReverseZ()) return farClear ? 0.0f : float((1.0 - g) * double(kPicaDepthMax));
  return farClear ? kPicaDepthClearFar : float(g * double(kPicaDepthMax));
}
inline void picaFoldDepthRow(float row2[4], const float row3[4], float s, float o) {
  for (int j = 0; j < 4; j++) row2[j] = float(-(double(s) * row2[j] + double(o) * row3[j]));
}
struct PicaDepthMap {
  bool zBuffer = true;
  float scale = 1.0f, offset = 1.0f;
  uint32_t clear = 0xFFFFFF;   // 24-bit depth the target is cleared to (= depth 1.0, farthest)
};
PicaDepthMap depthMapZ();
PicaDepthMap depthMapW(float rangeD);
float picaDepthValue(const PicaDepthMap&, float zOverW, float wClip);   // [0,1], before the 24-bit quantisation
uint32_t picaDepth24(const PicaDepthMap&, float zOverW, float wClip);

// ---- fog. GX: out = lerp(color, fogColor, f(d)),  d = eye-space distance along the view axis.
// PICA: out = mix(fogColor, color, lut(depth)),  depth in [0,1) indexes a 128-entry LUT with linear interpolation, so
// the port uses W-buffering with depth = w_clip / D (D = depthRange) and fills the LUT with 1 - f(i * D / 128).
struct GxFog { uint8_t type = 0; float startZ = 0, endZ = 1, nearZ = 0.1f, farZ = 1; uint8_t r = 0, g = 0, b = 0; };
// GX types: NONE 0, PERSP_LIN 2, PERSP_EXP 4, PERSP_EXP2 5, PERSP_REVEXP 6, PERSP_REVEXP2 7; +8 = ORTHO variants.
float fogFactorGx(uint8_t type, float t);  // fog strength for t = (d - start) / (end - start), clamped inside; 0 = clear, 1 = fully fogged
struct PicaFog {
  bool enable = false;
  uint32_t lut[128] = {};     // citro3d C3D_FogLut::data layout: value (11 bits) << 13 | signed difference (13 bits)
  uint32_t color = 0;         // C3D_FogColor: 0xBBGGRR
  float depthRange = 1;       // D: world units mapped to depth 1.0
  float depthScale = 0, depthOffset = 1;   // C3D_DepthMap(false /*W-buffer*/, depthScale, depthOffset): depth = z*scale + w*offset
  bool depthSaturates = false;             // true if geometry beyond D shares depth 1.0
  float maxErr255 = 0;        // worst |PICA fog factor - exact| over [0, D], in 1/255 units
  bool exact = true;
  std::string note;
};
// projFar: far distance of the active projection (W-buffer range upper bound). shrink=true lets the policy lower D so
// the LUT cells stay small relative to the fog ramp (about 32 cells across it; see the .cpp).
// forceRange > 0: the frame policy fixed D for every draw of the frame (see PicaDepthMap); `shrink` is then ignored.
PicaFog fogFromGx(const GxFog&, float projFar, bool shrink = true, float forceRange = 0.0f);
// Same range policy as fogFromGx, without generating a LUT or measuring error.
float fogDepthRange(const GxFog&, float projFar, bool shrink = true, float forceRange = 0.0f);

// Renderer-owned, bounded cache of identical fog preparation. Colour is patched
// per request; it does not affect the LUT, depth policy, or error measurement.
class FogCompileCache {
 public:
  PicaFog get(const GxFog&, float projFar, bool shrink = true, float forceRange = 0.0f);
 private:
  struct Entry { uint32_t key[9] = {}; PicaFog value; uint64_t age = 0; bool valid = false; };
  Entry entries_[16];
  uint64_t age_ = 0;
};

// factor the GPU computes for a pixel at this depth (1 = unfogged). lutScale: 2048 = libctru's encoding (11-bit fraction);
// Azahar decodes entries with 2047, pass 2047 to predict its output exactly.
float picaFogEval(const PicaFog&, float depth01, float lutScale = 2048.0f);

std::string describe(const GxAlphaCompare&, const PicaAlphaTest&);
std::string describe(const GxBlend&, const PicaBlend&);

}  // namespace t2p
