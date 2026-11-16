// Test-support code shared by tests/test_vtx.cpp and tools/gen_hwcheck3.cpp: deterministic RNG and randomised GX state.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "vtx.h"

namespace t2p {

struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  explicit Rng(uint64_t seed = 1) { s ^= seed * 0xD1B54A32D192ED03ull; for (int i = 0; i < 8; i++) next(); }
  uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
  float f() { return float(next() >> 40) / float(1 << 24); }             // [0,1)
  float r(float lo, float hi) { return lo + (hi - lo) * f(); }
  int i(int n) { return int(next() % uint64_t(n)); }
};


static inline void rotMat(Rng& g, float R[3][3]) {
  float q[4], n = 0;
  for (float& v : q) { v = g.r(-1, 1); n += v * v; }
  n = std::sqrt(n);
  for (float& v : q) v /= n;
  float x = q[0], y = q[1], z = q[2], w = q[3];
  R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - z * w); R[0][2] = 2 * (x * z + y * w);
  R[1][0] = 2 * (x * y + z * w); R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - x * w);
  R[2][0] = 2 * (x * z - y * w); R[2][1] = 2 * (y * z + x * w); R[2][2] = 1 - 2 * (x * x + y * y);
}

// SDK light helpers (GXInitLightSpot / GXInitLightDistAttn)
static inline void spotCoeffs(Rng& g, float a[3], bool realistic) {
  static const int kRealistic[3] = {6, 1, 2};   // SP_OFF, SP_COS, SP_COS2: the cone functions without huge coefficients
  int fn = realistic ? kRealistic[g.i(3)] : g.i(7);
  float cutoff = realistic ? g.r(30, 85) : g.r(8, 85);
  if (fn == 6) { a[0] = 1; a[1] = 0; a[2] = 0; return; }
  float rad = std::cos(3.1415927f * cutoff / 180.0f), b, c, d, aa;
  switch (fn) {
    case 0: aa = -1000.0f * rad; b = 1000.0f; c = 0; break;
    case 1: b = 1.0f / (1.0f - rad); aa = -rad * b; c = 0; break;
    case 2: c = 1.0f / (1.0f - rad); aa = 0; b = -rad * c; break;
    case 3: d = 1.0f / ((1.0f - rad) * (1.0f - rad)); aa = rad * (rad - 2.0f) * d; b = 2.0f * d; c = -d; break;
    case 4: d = 1.0f / ((1.0f - rad) * (1.0f - rad)); c = -4.0f * d; aa = c * rad; b = 4.0f * (1.0f + rad) * d; break;
    default: d = 1.0f / ((1.0f - rad) * (1.0f - rad)); aa = 1.0f - 2.0f * rad * rad * d; b = 4.0f * rad * d; c = -2.0f * d; break;
  }
  a[0] = aa; a[1] = b; a[2] = c;
}
static inline void distCoeffs(Rng& g, float k[3]) {
  int fn = g.i(4);
  float dist = g.r(40, 400), br = g.r(0.1f, 0.9f);
  switch (fn) {
    case 0: k[0] = 1; k[1] = (1 - br) / (br * dist); k[2] = 0; break;
    case 1: k[0] = 1; k[1] = 0.5f * (1 - br) / (br * dist); k[2] = 0.5f * (1 - br) / (dist * (br * dist)); break;
    case 2: k[0] = 1; k[1] = 0; k[2] = (1 - br) / (dist * (br * dist)); break;
    default: k[0] = 1; k[1] = 0; k[2] = 0; break;
  }
}

static inline void randomUniforms(Rng& g, VtxUniforms& U, int nmtx, bool realistic) {
  float P[4][4] = {{g.r(0.5f, 3), 0, g.r(-0.3f, 0.3f), 0}, {0, g.r(0.5f, 3), g.r(-0.3f, 0.3f), 0}, {0, 0, g.r(-1.1f, -0.9f), g.r(-20, -1)}, {0, 0, -1, 0}};
  std::memcpy(U.proj, P, sizeof P);
  for (int m = 0; m < nmtx; m++) {
    float R[3][3];
    rotMat(g, R);
    float sc = g.r(0.5f, 2.0f);
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 3; c++) { U.pos[m][r][c] = R[r][c] * sc; U.nrm[m][r][c] = R[r][c] / sc * g.r(0.9f, 1.1f); }
      U.pos[m][r][3] = g.r(-50, 50);
      U.nrm[m][r][3] = 0;
    }
  }
  const int ambMax = g.i(2) ? 72 : 256;   // mostly leave headroom below the 255 clamp so the light math is visible
  for (int c = 0; c < 4; c++) { U.matColor[c] = float(g.i(256)); U.ambColor[c] = float(g.i(ambMax)); }
  for (int l = 0; l < 8; l++) {
    VtxLight& L = U.light[l];
    for (int c = 0; c < 4; c++) L.color[c] = float(g.i(256));
    const bool far = realistic && g.i(2);   // the game also places a "sun" light tens of thousands of units away
    for (int c = 0; c < 3; c++) L.pos[c] = far ? g.r(-40000, 40000) : g.r(-60, 60);
    float d[3], n = 0;
    for (float& v : d) { v = g.r(-1, 1); n += v * v; }
    n = std::sqrt(n);
    for (int c = 0; c < 3; c++) L.dir[c] = d[c] / n;
    spotCoeffs(g, L.cosatt, realistic);
    distCoeffs(g, L.distatt);
  }
  for (int m = 0; m < 10; m++)
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 4; c++) U.texMtx[m][r][c] = g.r(-2, 2);
  for (int u = 0; u < 3; u++) { U.texScale[u][0] = g.r(0.1f, 1.0f); U.texScale[u][1] = g.r(0.1f, 1.0f); }
}

static inline void randomInput(Rng& g, VtxInput& I, int nmtx) {
  for (float& v : I.pos) v = g.r(-100, 100);
  float n = 0;
  for (float& v : I.nrm) { v = g.r(-1, 1); n += v * v; }
  n = std::sqrt(n);
  for (float& v : I.nrm) v /= n;
  for (float& v : I.clr0) v = float(g.i(256));
  for (auto& t : I.tex) { t[0] = g.r(-2, 3); t[1] = g.r(-2, 3); }
  I.mtxIdx = g.i(nmtx);
}


}  // namespace t2p
