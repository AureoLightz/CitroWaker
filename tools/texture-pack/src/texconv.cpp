#ifdef __3DS__
#include <3ds.h>
#endif
#include <chrono>
#include "texconv.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace t2p {
namespace {

inline uint8_t ex3(uint32_t n) { return uint8_t((n << 5) | (n << 2) | (n >> 1)); }
inline uint8_t ex4(uint32_t n) { return uint8_t((n << 4) | n); }
inline uint8_t ex5(uint32_t n) { return uint8_t((n << 3) | (n >> 2)); }
inline uint8_t ex6(uint32_t n) { return uint8_t((n << 2) | (n >> 4)); }
inline uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

struct BlockGeom { int bw, bh, bytes; };
bool geom(int fmt, BlockGeom& g) {
  switch (fmt) {
    case GXF_I4: case GXF_C4: g = {8, 8, 32}; return true;
    case GXF_I8: case GXF_IA4: case GXF_C8: g = {8, 4, 32}; return true;
    case GXF_IA8: case GXF_RGB565: case GXF_RGB5A3: case GXF_C14X2: g = {4, 4, 32}; return true;
    case GXF_RGBA8: g = {4, 4, 64}; return true;
    case GXF_CMPR: g = {8, 8, 32}; return true;
  }
  return false;
}

void put(uint8_t* o, uint32_t r, uint32_t g, uint32_t b, uint32_t a) { o[0] = uint8_t(r); o[1] = uint8_t(g); o[2] = uint8_t(b); o[3] = uint8_t(a); }

void decode565(uint32_t v, uint8_t* o) { put(o, ex5(v >> 11 & 31), ex6(v >> 5 & 63), ex5(v & 31), 255); }
void decode5A3(uint32_t v, uint8_t* o) {
  if (v & 0x8000) put(o, ex5(v >> 10 & 31), ex5(v >> 5 & 31), ex5(v & 31), 255);
  else put(o, ex4(v >> 8 & 15), ex4(v >> 4 & 15), ex4(v & 15), ex3(v >> 12 & 7));
}

// GC DXT1 colour table (4 entries of RGBA)
void cmprTable(const uint8_t* b, uint8_t t[16]) {
  const uint32_t c1 = be16(b), c2 = be16(b + 2);
  decode565(c1, t);
  decode565(c2, t + 4);
  if (c1 > c2) {   // hardware weights are 3/8 and 5/8 (not 1/3, 2/3)
    for (int k = 0; k < 3; k++) {
      t[8 + k] = uint8_t((3 * t[4 + k] + 5 * t[k]) >> 3);
      t[12 + k] = uint8_t((3 * t[k] + 5 * t[4 + k]) >> 3);
    }
    t[11] = t[15] = 255;
  } else {
    for (int k = 0; k < 3; k++) { t[8 + k] = uint8_t((t[k] + t[4 + k]) >> 1); t[12 + k] = t[8 + k]; }
    t[11] = 255;
    t[15] = 0;       // index 3: midpoint colour, transparent
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------ GX decode
GxTlut makeGxTlut(int tlutFmt, const uint8_t* d, int entries) {
  GxTlut t;
  t.rgba.resize(size_t(entries) * 4);
  for (int i = 0; i < entries; i++) {
    uint8_t* o = &t.rgba[size_t(i) * 4];
    const uint32_t v = be16(d + 2 * i);
    if (tlutFmt == GXT_IA8) { const uint8_t a = uint8_t(v >> 8), in = uint8_t(v & 255); put(o, in, in, in, a); }
    else if (tlutFmt == GXT_RGB565) decode565(v, o);
    else decode5A3(v, o);
  }
  return t;
}

bool gxFormatHasPalette(int fmt) { return fmt == GXF_C4 || fmt == GXF_C8 || fmt == GXF_C14X2; }

size_t gxTextureBytes(int fmt, int w, int h) {
  BlockGeom g;
  if (!geom(fmt, g)) return 0;
  return size_t((w + g.bw - 1) / g.bw) * size_t((h + g.bh - 1) / g.bh) * size_t(g.bytes);
}

bool decodeGxTexture(int fmt, int w, int h, const uint8_t* data, size_t size, const GxTlut* tlut, std::vector<uint8_t>& rgba, std::string& err) {
  BlockGeom g;
  if (!geom(fmt, g)) { err = "unknown GX texture format " + std::to_string(fmt); return false; }
  if (w <= 0 || h <= 0) { err = "bad size"; return false; }
  if (size < gxTextureBytes(fmt, w, h)) { err = "texture data is shorter than the format needs"; return false; }
  if (gxFormatHasPalette(fmt) && (!tlut || tlut->rgba.empty())) { err = "palette texture without a TLUT"; return false; }
  rgba.assign(size_t(w) * h * 4, 0);
  const size_t entries = tlut ? tlut->rgba.size() / 4 : 0;
  const uint8_t* in = data;
  for (int by = 0; by < (h + g.bh - 1) / g.bh; by++)
    for (int bx = 0; bx < (w + g.bw - 1) / g.bw; bx++, in += g.bytes) {
      for (int y = 0; y < g.bh; y++)
        for (int x = 0; x < g.bw; x++) {
          const int px = bx * g.bw + x, py = by * g.bh + y;
          if (px >= w || py >= h) continue;
          uint8_t* o = &rgba[(size_t(py) * w + px) * 4];
          switch (fmt) {
            case GXF_I4: { const uint8_t b = in[(y * 8 + x) / 2]; const uint8_t i = ex4((x & 1) ? (b & 15) : (b >> 4)); put(o, i, i, i, i); break; }
            case GXF_I8: { const uint8_t i = in[y * 8 + x]; put(o, i, i, i, i); break; }
            case GXF_IA4: { const uint8_t b = in[y * 8 + x]; const uint8_t i = ex4(b & 15); put(o, i, i, i, ex4(b >> 4)); break; }
            case GXF_IA8: { const uint8_t* p = in + (y * 4 + x) * 2; put(o, p[1], p[1], p[1], p[0]); break; }
            case GXF_RGB565: decode565(be16(in + (y * 4 + x) * 2), o); break;
            case GXF_RGB5A3: decode5A3(be16(in + (y * 4 + x) * 2), o); break;
            case GXF_RGBA8: { const uint8_t* p = in + (y * 4 + x) * 2; const uint8_t* q = in + 32 + (y * 4 + x) * 2; put(o, p[1], q[0], q[1], p[0]); break; }
            case GXF_C4: case GXF_C8: case GXF_C14X2: {
              size_t idx;
              if (fmt == GXF_C4) { const uint8_t b = in[(y * 8 + x) / 2]; idx = (x & 1) ? (b & 15) : (b >> 4); }
              else if (fmt == GXF_C8) idx = in[y * 8 + x];
              else idx = be16(in + (y * 4 + x) * 2) & 0x3fff;
              if (idx >= entries) put(o, 0, 0, 0, 0);   // beyond the loaded palette the hardware reads stale TMEM; Aurora uses transparent black
              else std::memcpy(o, &tlut->rgba[idx * 4], 4);
              break;
            }
            case GXF_CMPR: {
              const uint8_t* sb = in + ((y / 4) * 2 + (x / 4)) * 8;
              uint8_t t[16];
              cmprTable(sb, t);
              const int idx = (sb[4 + (y & 3)] >> (6 - 2 * (x & 3))) & 3;
              std::memcpy(o, t + idx * 4, 4);
              break;
            }
          }
        }
    }
  return true;
}

// ------------------------------------------------------------------------------------------------ PICA side
const char* picaTexFmtName(int f) {
  static const char* n[] = {"RGBA8", "RGB8", "RGB5A1", "RGB565", "RGBA4", "IA8", "HILO8", "I8", "A8", "IA4", "I4", "A4", "ETC1", "ETC1A4"};
  return f >= 0 && f < 14 ? n[f] : "?";
}

static size_t tileBytes(int fmt) {
  switch (fmt) {
    case PTF_RGBA8: return 256;
    case PTF_RGB8: return 192;
    case PTF_RGB5A1: case PTF_RGB565: case PTF_RGBA4: case PTF_IA8: case PTF_HILO8: return 128;
    case PTF_I8: case PTF_A8: case PTF_IA4: return 64;
    case PTF_I4: case PTF_A4: return 32;
    case PTF_ETC1: return 32;
    case PTF_ETC1A4: return 64;
  }
  return 0;
}
size_t picaTextureBytes(int fmt, int w, int h) { return size_t(w / 8) * size_t(h / 8) * tileBytes(fmt); }

static inline int morton(int x, int y) {
  static const int xl[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
  static const int yl[] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
  return xl[x & 7] + yl[y & 7];
}

static const int kEtc1Mod[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};

static void etc1Decode(uint64_t v, int x, int y, uint8_t rgb[3]) {   // x column, y row (0..3), Azahar's ETC1Tile::GetRGB
  const int texel = 4 * x + y;
  const bool flip = (v >> 32) & 1, diff = (v >> 33) & 1;
  if (flip) std::swap(x, y);
  int c[3];
  if (diff) {
    const int r = int(v >> 59 & 31), g = int(v >> 51 & 31), b = int(v >> 43 & 31);
    int dr = int(v >> 56 & 7), dg = int(v >> 48 & 7), db = int(v >> 40 & 7);
    if (dr >= 4) dr -= 8;
    if (dg >= 4) dg -= 8;
    if (db >= 4) db -= 8;
    c[0] = r; c[1] = g; c[2] = b;
    if (x >= 2) { c[0] += dr; c[1] += dg; c[2] += db; }
    for (int k = 0; k < 3; k++) c[k] = ex5(uint32_t(c[k]) & 31);
  } else if (x < 2) {
    c[0] = ex4(uint32_t(v >> 60 & 15)); c[1] = ex4(uint32_t(v >> 52 & 15)); c[2] = ex4(uint32_t(v >> 44 & 15));
  } else {
    c[0] = ex4(uint32_t(v >> 56 & 15)); c[1] = ex4(uint32_t(v >> 48 & 15)); c[2] = ex4(uint32_t(v >> 40 & 15));
  }
  const int table = int((x < 2) ? (v >> 37 & 7) : (v >> 34 & 7));
  int mod = kEtc1Mod[table][(v >> texel) & 1];
  if ((v >> (16 + texel)) & 1) mod = -mod;
  for (int k = 0; k < 3; k++) rgb[k] = uint8_t(std::min(255, std::max(0, c[k] + mod)));
}

void picaSampleTexel(int fmt, const uint8_t* data, int w, int x, int y, uint8_t o[4]) {
  const uint8_t* tile = data + (size_t(y / 8) * size_t(w / 8) + size_t(x / 8)) * tileBytes(fmt);
  const int fx = x & 7, fy = y & 7, m = morton(fx, fy);
  switch (fmt) {
    case PTF_RGBA8: { const uint8_t* p = tile + m * 4; put(o, p[3], p[2], p[1], p[0]); break; }
    case PTF_RGB8: { const uint8_t* p = tile + m * 3; put(o, p[2], p[1], p[0], 255); break; }
    case PTF_RGB5A1: { const uint8_t* p = tile + m * 2; const uint32_t v = p[0] | (p[1] << 8); put(o, ex5(v >> 11 & 31), ex5(v >> 6 & 31), ex5(v >> 1 & 31), (v & 1) ? 255 : 0); break; }
    case PTF_RGB565: { const uint8_t* p = tile + m * 2; decode565(p[0] | (p[1] << 8), o); break; }
    case PTF_RGBA4: { const uint8_t* p = tile + m * 2; const uint32_t v = p[0] | (p[1] << 8); put(o, ex4(v >> 12 & 15), ex4(v >> 8 & 15), ex4(v >> 4 & 15), ex4(v & 15)); break; }
    case PTF_IA8: { const uint8_t* p = tile + m * 2; put(o, p[1], p[1], p[1], p[0]); break; }
    case PTF_HILO8: { const uint8_t* p = tile + m * 2; put(o, p[1], p[0], 0, 255); break; }
    case PTF_I8: { const uint8_t i = tile[m]; put(o, i, i, i, 255); break; }
    case PTF_A8: put(o, 0, 0, 0, tile[m]); break;
    case PTF_IA4: { const uint8_t b = tile[m]; const uint8_t i = ex4(b >> 4); put(o, i, i, i, ex4(b & 15)); break; }
    case PTF_I4: { const uint8_t b = tile[m / 2]; const uint8_t i = ex4((m & 1) ? (b >> 4) : (b & 15)); put(o, i, i, i, 255); break; }
    case PTF_A4: { const uint8_t b = tile[m / 2]; put(o, 0, 0, 0, ex4((m & 1) ? (b >> 4) : (b & 15))); break; }
    case PTF_ETC1: case PTF_ETC1A4: {
      const bool al = fmt == PTF_ETC1A4;
      const size_t sub = size_t((fx / 4) + 2 * (fy / 4)) * (al ? 16 : 8);
      const uint8_t* p = tile + sub;
      uint8_t a = 255;
      int ex = fx & 3, ey = fy & 3;
      if (al) {
        uint64_t pa = 0;
        for (int k = 7; k >= 0; k--) pa = (pa << 8) | p[k];
        a = ex4(uint32_t(pa >> (4 * (ex * 4 + ey))) & 15);
        p += 8;
      }
      uint64_t v = 0;
      for (int k = 7; k >= 0; k--) v = (v << 8) | p[k];
      etc1Decode(v, ex, ey, o);
      o[3] = a;
      break;
    }
    default: put(o, 0, 0, 0, 0);
  }
}

std::vector<uint8_t> picaDecodeImage(int fmt, const uint8_t* data, int w, int h) {
  std::vector<uint8_t> out(size_t(w) * h * 4);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) picaSampleTexel(fmt, data, w, x, y, &out[(size_t(y) * w + x) * 4]);
  return out;
}

// ---- lossless / simple encoders
static inline uint32_t q(uint32_t v, int bits) { const uint32_t m = (1u << bits) - 1; return (v * m + 127) / 255; }

static void encodeTexel(int fmt, const uint8_t* s, uint8_t* tile, int m) {
  const uint32_t r = s[0], g = s[1], b = s[2], a = s[3];
  const uint32_t grey = (r + g + b + 1) / 3;
  switch (fmt) {
    case PTF_RGBA8: tile[m * 4] = uint8_t(a); tile[m * 4 + 1] = uint8_t(b); tile[m * 4 + 2] = uint8_t(g); tile[m * 4 + 3] = uint8_t(r); break;
    case PTF_RGB8: tile[m * 3] = uint8_t(b); tile[m * 3 + 1] = uint8_t(g); tile[m * 3 + 2] = uint8_t(r); break;
    case PTF_RGB5A1: { const uint32_t v = (q(r, 5) << 11) | (q(g, 5) << 6) | (q(b, 5) << 1) | (a >= 128 ? 1 : 0); tile[m * 2] = uint8_t(v); tile[m * 2 + 1] = uint8_t(v >> 8); break; }
    case PTF_RGB565: { const uint32_t v = (q(r, 5) << 11) | (q(g, 6) << 5) | q(b, 5); tile[m * 2] = uint8_t(v); tile[m * 2 + 1] = uint8_t(v >> 8); break; }
    case PTF_RGBA4: { const uint32_t v = (q(r, 4) << 12) | (q(g, 4) << 8) | (q(b, 4) << 4) | q(a, 4); tile[m * 2] = uint8_t(v); tile[m * 2 + 1] = uint8_t(v >> 8); break; }
    case PTF_IA8: tile[m * 2] = uint8_t(a); tile[m * 2 + 1] = uint8_t(grey); break;
    case PTF_I8: tile[m] = uint8_t(grey); break;
    case PTF_A8: tile[m] = uint8_t(a); break;
    case PTF_IA4: tile[m] = uint8_t((q(grey, 4) << 4) | q(a, 4)); break;
    case PTF_I4: { uint8_t& d = tile[m / 2]; const uint8_t v = uint8_t(q(grey, 4)); d = (m & 1) ? uint8_t((d & 15) | (v << 4)) : uint8_t((d & 0xF0) | v); break; }
    case PTF_A4: { uint8_t& d = tile[m / 2]; const uint8_t v = uint8_t(q(a, 4)); d = (m & 1) ? uint8_t((d & 15) | (v << 4)) : uint8_t((d & 0xF0) | v); break; }
  }
}

// (Claude) What encodeTexel + picaSampleTexel return for one texel, written out per format (same q / ex4 / ex5 / ex6 and the
// same grey), so the exact-format scan needs no tile addressing. Checked against the generic pair by texconvSelfTest.
namespace {
struct RoundTrip { uint8_t r4[256], r5[256], r6[256]; RoundTrip() { for (uint32_t v = 0; v < 256; v++) { r4[v] = ex4(q(v, 4)); r5[v] = ex5(q(v, 5)); r6[v] = ex6(q(v, 6)); } } };
const RoundTrip& roundTrip() { static const RoundTrip t; return t; }
// decoded texel of `fmt` for source texel s (the formats of chooseFormatLevels' exact order)
inline void picaRoundTrip(int fmt, const uint8_t* s, uint8_t o[4]) {
  const RoundTrip& T = roundTrip();
  const uint32_t r = s[0], g = s[1], b = s[2], a = s[3];
  const uint32_t grey = (r + g + b + 1) / 3;
  switch (fmt) {
    case PTF_RGBA8: o[0] = uint8_t(r); o[1] = uint8_t(g); o[2] = uint8_t(b); o[3] = uint8_t(a); break;
    case PTF_RGB8: o[0] = uint8_t(r); o[1] = uint8_t(g); o[2] = uint8_t(b); o[3] = 255; break;
    case PTF_RGB5A1: o[0] = T.r5[r]; o[1] = T.r5[g]; o[2] = T.r5[b]; o[3] = a >= 128 ? 255 : 0; break;
    case PTF_RGB565: o[0] = T.r5[r]; o[1] = T.r6[g]; o[2] = T.r5[b]; o[3] = 255; break;
    case PTF_RGBA4: o[0] = T.r4[r]; o[1] = T.r4[g]; o[2] = T.r4[b]; o[3] = T.r4[a]; break;
    case PTF_IA8: o[0] = o[1] = o[2] = uint8_t(grey); o[3] = uint8_t(a); break;
    case PTF_I8: o[0] = o[1] = o[2] = uint8_t(grey); o[3] = 255; break;
    case PTF_A8: o[0] = o[1] = o[2] = 0; o[3] = uint8_t(a); break;
    case PTF_IA4: o[0] = o[1] = o[2] = T.r4[grey]; o[3] = T.r4[a]; break;
    case PTF_I4: o[0] = o[1] = o[2] = T.r4[grey]; o[3] = 255; break;
    case PTF_A4: o[0] = o[1] = o[2] = 0; o[3] = T.r4[a]; break;
    default: o[0] = o[1] = o[2] = o[3] = 0;
  }
}
// Non-ETC trial formats have independent channels. Store their exact absolute
// errors, avoiding a round-trip switch and 64-bit accumulation per channel.
struct ErrorTables {
  uint8_t e4[256],e5[256],e6[256],e1[256],opaque[256];
  ErrorTables(){const auto& t=roundTrip();for(int v=0;v<256;++v){
    e4[v]=uint8_t(std::abs(v-int(t.r4[v])));e5[v]=uint8_t(std::abs(v-int(t.r5[v])));e6[v]=uint8_t(std::abs(v-int(t.r6[v])));
    e1[v]=uint8_t(std::abs(v-(v>=128?255:0)));opaque[v]=uint8_t(255-v);
  }}
};
const ErrorTables& errorTables(){static const ErrorTables t;return t;}
struct ErrorChannels {const uint8_t *r,*g,*b,*a;};
ErrorChannels errorChannels(int fmt){const auto& t=errorTables();
  if(fmt==PTF_RGBA4)return {t.e4,t.e4,t.e4,t.e4};
  if(fmt==PTF_RGB5A1)return {t.e5,t.e5,t.e5,t.e1};
  return {t.e5,t.e6,t.e5,t.opaque}; // RGB565, the only other non-ETC trial
}
template<bool Mapped,bool Alpha>
bool measureErrorTable(const MipImage& m,const ErrorChannels& e,double maxLimit,uint32_t& mx,uint64_t& sum){
  mx=0;sum=0;
  for(int y=0;y<m.h;++y){
    const uint8_t* row=m.rgba.data()+size_t(Mapped?m.sourceY[y]:y)*size_t(Mapped?m.sourceWidth:m.w)*4;
    for(int x=0;x<m.w;){
      uint32_t partial=0;const int end=x+std::min(256,m.w-x);
      for(;x<end;++x){const uint8_t* s=row+size_t(Mapped?m.sourceX[x]:x)*4;
        const uint32_t r=e.r[s[0]],g=e.g[s[1]],b=e.b[s[2]];
        partial+=r+g+b;mx=std::max(mx,std::max(r,std::max(g,b)));
        if(Alpha){const uint32_t a=e.a[s[3]];partial+=a;mx=std::max(mx,a);}
      }
      // A maximum violation can never be repaired by later texels or levels.
      // Only rejected candidates stop early; accepted statistics are unchanged.
      if(double(mx)>maxLimit)return false;
      sum+=partial;
    }
  }
  return true;
}
bool measureErrorTable(const MipImage& m,int fmt,bool alpha,double limit,uint32_t& mx,uint64_t& sum){
 const auto e=errorChannels(fmt);
 if(m.sourceX.empty())return alpha?measureErrorTable<false,true>(m,e,limit,mx,sum):measureErrorTable<false,false>(m,e,limit,mx,sum);
 return alpha?measureErrorTable<true,true>(m,e,limit,mx,sum):measureErrorTable<true,false>(m,e,limit,mx,sum);
}
}  // namespace
// (Claude) Which formats of chooseFormatLevels' exact order cannot hold texel s exactly, as one bit each (bit i = kExactOrder[i]):
// the comparison of picaRoundTrip's result with s, written as conditions on the channels. Checked by texconvSelfTest.
static const int kExactOrder[] = {PTF_I4, PTF_A4, PTF_IA4, PTF_I8, PTF_A8, PTF_RGB565, PTF_RGB5A1, PTF_RGBA4, PTF_IA8, PTF_RGB8, PTF_RGBA8};
namespace {
struct ChanFlags { uint8_t f[256]; ChanFlags() { const RoundTrip& T = roundTrip(); for (uint32_t v = 0; v < 256; v++) f[v] = uint8_t((T.r4[v] != v ? 1 : 0) | (T.r5[v] != v ? 2 : 0) | (T.r6[v] != v ? 4 : 0)); } };
inline uint32_t inexactMask(const uint8_t* s, bool needAlpha) {
  static const ChanFlags C;
  const uint32_t r = s[0], g = s[1], b = s[2], a = s[3];
  const uint32_t fr = C.f[r], fg = C.f[g], fb = C.f[b], fa = C.f[a];
  const bool grey = r == g && g == b;          // an intensity format returns (r + g + b + 1) / 3 in all three
  const bool zero = (r | g | b) == 0;          // the alpha-only formats return black
  const bool n4 = (fr & 1) != 0;               // (for grey texels) the intensity does not survive 4 bits
  const bool a4 = needAlpha && (fa & 1);       // alpha does not survive 4 bits
  const bool aOpaque = needAlpha && a != 255;  // formats without alpha return 255
  const bool aBin = needAlpha && a != 0 && a != 255;   // RGB5A1 returns 0 or 255 (a >= 128)
  uint32_t m = 0;
  if (!grey || n4 || aOpaque) m |= 1u << 0;    // I4
  if (!zero || a4) m |= 1u << 1;               // A4
  if (!grey || n4 || a4) m |= 1u << 2;         // IA4
  if (!grey || aOpaque) m |= 1u << 3;          // I8
  if (!zero) m |= 1u << 4;                     // A8
  if ((fr & 2) || (fg & 4) || (fb & 2) || aOpaque) m |= 1u << 5;   // RGB565
  if (((fr | fg | fb) & 2) || aBin) m |= 1u << 6;                  // RGB5A1
  if (((fr | fg | fb) & 1) || a4) m |= 1u << 7;                    // RGBA4
  if (!grey) m |= 1u << 8;                     // IA8
  if (aOpaque) m |= 1u << 9;                   // RGB8
  return m;                                    // RGBA8 (bit 10) always holds it
}
}  // namespace
#ifdef TEXCONV_SELFTEST
// every (r, g, b) with several alphas, and every grey with every alpha, for each format: the fast round trip = encodeTexel + picaSampleTexel
long texconvSelfTest() {
  static const int fmts[] = {PTF_I4, PTF_A4, PTF_IA4, PTF_I8, PTF_A8, PTF_RGB565, PTF_RGB5A1, PTF_RGBA4, PTF_IA8, PTF_RGB8, PTF_RGBA8};
  long bad = 0, n = 0;
  auto check = [&](const uint8_t s[4]) {
    for (int f : fmts) {
      uint8_t tile[256] = {}, g[4], fst[4];
      encodeTexel(f, s, tile, 0); picaSampleTexel(f, tile, 8, 0, 0, g); picaRoundTrip(f, s, fst); n++;
      if(f==PTF_RGBA4||f==PTF_RGB5A1||f==PTF_RGB565){const auto e=errorChannels(f);const uint8_t* channels[]={e.r,e.g,e.b,e.a};
        for(int c=0;c<4;++c)if(channels[c][s[c]]!=std::abs(int(s[c])-int(g[c]))&&bad++<10)std::fprintf(stderr,"selftest error table fmt %d channel %d\n",f,c);
      }
      for (int na = 0; na < 2; na++) {
        const bool differs = fst[0] != s[0] || fst[1] != s[1] || fst[2] != s[2] || (na && fst[3] != s[3]);
        int idx = 0; while (kExactOrder[idx] != f) idx++;
        if (differs != ((inexactMask(s, na != 0) >> idx & 1) != 0) && bad++ < 10) std::fprintf(stderr, "selftest mask fmt %d needAlpha %d src %d %d %d %d\n", f, na, s[0], s[1], s[2], s[3]);
      }
      if (std::memcmp(g, fst, 4) != 0 && bad++ < 10) std::fprintf(stderr, "selftest fmt %d src %d %d %d %d: generic %d %d %d %d fast %d %d %d %d\n", f, s[0], s[1], s[2], s[3], g[0], g[1], g[2], g[3], fst[0], fst[1], fst[2], fst[3]);
    }
  };
  static const int alphas[] = {0, 1, 7, 8, 127, 128, 200, 254, 255};
  for (int r = 0; r < 256; r++) for (int g = 0; g < 256; g++) for (int b = 0; b < 256; b++) for (int a : alphas) { const uint8_t s[4] = {uint8_t(r), uint8_t(g), uint8_t(b), uint8_t(a)}; check(s); }
  for (int v = 0; v < 256; v++) for (int a = 0; a < 256; a++) { const uint8_t s[4] = {uint8_t(v), uint8_t(v), uint8_t(v), uint8_t(a)}; check(s); }
  std::fprintf(stderr, "texconv selftest: %ld texel/format pairs, %ld differ\n", n, bad);
  return bad;
}
#endif

bool picaEncode(int fmt, const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& out, bool compareAlpha, bool verify) {
  out.assign(picaTextureBytes(fmt, w, h), 0);
  const size_t tb = tileBytes(fmt);
  bool exact = true;
  if (((w | h) & 7) == 0) {   // (Claude) tile by tile, in Morton order: the same bytes as the texel-order loop below
    static const struct Mo { uint8_t x[64], y[64]; Mo() { for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) { const int m = morton(xx, yy); x[m] = uint8_t(xx); y[m] = uint8_t(yy); } } } mo;
    uint8_t* tile = out.data();
    for (int ty = 0; ty < h; ty += 8)
      for (int tx = 0; tx < w; tx += 8, tile += tb) {
        const uint8_t* base = &rgba[(size_t(ty) * w + tx) * 4];
        for (int m = 0; m < 64; m++) encodeTexel(fmt, base + (size_t(mo.y[m]) * w + mo.x[m]) * 4, tile, m);
      }
  } else
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t* tile = out.data() + (size_t(y / 8) * size_t(w / 8) + size_t(x / 8)) * tb;
      const uint8_t* s = &rgba[(size_t(y) * w + x) * 4];
      encodeTexel(fmt, s, tile, morton(x & 7, y & 7));
    }
  if (!verify) return true;   // the caller already knows (chooseFormatLevels' scan) or does not ask
  for (int y = 0; y < h && exact; y++)
    for (int x = 0; x < w; x++) {
      uint8_t o[4];
      picaSampleTexel(fmt, out.data(), w, x, y, o);
      const uint8_t* s = &rgba[(size_t(y) * w + x) * 4];
      if (o[0] != s[0] || o[1] != s[1] || o[2] != s[2] || (compareAlpha && o[3] != s[3])) { exact = false; break; }
    }
  return exact;
}

// ---- ETC1 encoder
namespace {

// One half-block (8 texels) coded with one base colour and one modifier table. sub/neg are 16-bit masks indexed by the texel's 4*x+y.
struct Cand { int q[3]; int table; int err; uint16_t sub, neg; };

Cand evalHalfTable(const uint8_t px[16][3], const int (*pts)[2], const int q[3], const int base[3], int table) {
  Cand c;
  for (int k = 0; k < 3; k++) c.q[k] = q[k];
  c.table = table; c.err = 0; c.sub = 0; c.neg = 0;
  for (int k = 0; k < 8; k++) {
    const int x = pts[k][0], y = pts[k][1];
    const uint8_t* p = px[4 * y + x];
    int bestE = 1 << 30, bs = 0, bn = 0;
    for (int col = 0; col < 2; col++)
      for (int sg = 0; sg < 2; sg++) {
        const int mod = sg ? -kEtc1Mod[table][col] : kEtc1Mod[table][col];
        int e = 0;
        for (int ch = 0; ch < 3; ch++) { const int d = std::min(255, std::max(0, base[ch] + mod)) - p[ch]; e += d * d; }
        if (e < bestE) { bestE = e; bs = col; bn = sg; }
      }
    c.err += bestE;
    c.sub = uint16_t(c.sub | (bs << (4 * x + y)));
    c.neg = uint16_t(c.neg | (bn << (4 * x + y)));
  }
  return c;
}

// Candidate codings of one half. The reconstruction of a texel is base + modifier, the same modifier on all three channels, so
// for a flat region the best bases are (average - one of the 32 signed modifiers) snapped to the base grid; quality 0 only tries
// the plain average (with every table).
std::vector<Cand> halfCandidates(const uint8_t px[16][3], const int (*pts)[2], bool differential, int quality) {
  double avg[3] = {};
  for (int k = 0; k < 8; k++) for (int c = 0; c < 3; c++) avg[c] += px[4 * pts[k][1] + pts[k][0]][c];
  for (int c = 0; c < 3; c++) avg[c] /= 8.0;
  const int levels = differential ? 31 : 15;
  auto quant = [&](double v) { return std::min(levels, std::max(0, int(std::lround(v * levels / 255.0)))); };
  auto expand = [&](int qv) { return differential ? int(ex5(uint32_t(qv))) : qv * 17; };
  std::vector<Cand> out;
  auto add = [&](const int qq[3], int table) {
    const int base[3] = {expand(qq[0]), expand(qq[1]), expand(qq[2])};
    out.push_back(evalHalfTable(px, pts, qq, base, table));
  };
  const int q0[3] = {quant(avg[0]), quant(avg[1]), quant(avg[2])};
  for (int t = 0; t < 8; t++) add(q0, t);
  if (quality > 0) {
    for (int t = 0; t < 8; t++)
      for (int col = 0; col < 2; col++)
        for (int sg = 0; sg < 2; sg++) {
          const double shift = sg ? -kEtc1Mod[t][col] : kEtc1Mod[t][col];
          const int qq[3] = {quant(avg[0] - shift), quant(avg[1] - shift), quant(avg[2] - shift)};
          if (qq[0] == q0[0] && qq[1] == q0[1] && qq[2] == q0[2]) continue;
          add(qq, t);
        }
  }
  return out;
}

}  // namespace

void etc1EncodeBlock(const uint8_t rgb[16][3], uint64_t& out, int quality) {
  int bestTotal = 1 << 30;
  uint64_t bestWord = 0;
  for (int flip = 0; flip < 2; flip++) {
    int pts[2][8][2];
    int n0 = 0, n1 = 0;
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++) {
        const int sel = flip ? y : x;   // first half: x < 2 (flip 0) or y < 2 (flip 1)
        if (sel < 2) { pts[0][n0][0] = x; pts[0][n0][1] = y; n0++; }
        else { pts[1][n1][0] = x; pts[1][n1][1] = y; n1++; }
      }
    // ---- individual mode (4-bit bases, independent halves)
    {
      const Cand* best[2] = {nullptr, nullptr};
      std::vector<Cand> cs[2] = {halfCandidates(rgb, pts[0], false, quality), halfCandidates(rgb, pts[1], false, quality)};
      for (int h = 0; h < 2; h++) for (const Cand& c : cs[h]) if (!best[h] || c.err < best[h]->err) best[h] = &c;
      const int total = best[0]->err + best[1]->err;
      if (total < bestTotal) {
        bestTotal = total;
        const Cand &a = *best[0], &b = *best[1];
        uint64_t v = uint64_t(a.sub | b.sub) | (uint64_t(a.neg | b.neg) << 16);   // each texel belongs to exactly one half
        v |= uint64_t(flip) << 32 | uint64_t(b.table) << 34 | uint64_t(a.table) << 37;
        v |= uint64_t(b.q[2]) << 40 | uint64_t(a.q[2]) << 44 | uint64_t(b.q[1]) << 48 | uint64_t(a.q[1]) << 52 | uint64_t(b.q[0]) << 56 | uint64_t(a.q[0]) << 60;
        bestWord = v;
      }
    }
    // ---- differential mode (5-bit base + 3-bit signed delta per channel, delta in [-4, 3])
    {
      std::vector<Cand> cs[2] = {halfCandidates(rgb, pts[0], true, quality), halfCandidates(rgb, pts[1], true, quality)};
      int bi = -1, bj = -1, be = 1 << 30;
      for (size_t i = 0; i < cs[0].size(); i++)
        for (size_t j = 0; j < cs[1].size(); j++) {
          bool ok = true;
          for (int c = 0; c < 3; c++) { const int d = cs[1][j].q[c] - cs[0][i].q[c]; if (d < -4 || d > 3) ok = false; }
          if (!ok) continue;
          const int e = cs[0][i].err + cs[1][j].err;
          if (e < be) { be = e; bi = int(i); bj = int(j); }
        }
      if (bi >= 0 && be < bestTotal) {
        bestTotal = be;
        const Cand& a = cs[0][size_t(bi)];
        const Cand& b = cs[1][size_t(bj)];
        uint64_t v = uint64_t(a.sub | b.sub) | (uint64_t(a.neg | b.neg) << 16);
        v |= uint64_t(flip) << 32 | uint64_t(1) << 33 | uint64_t(b.table) << 34 | uint64_t(a.table) << 37;
        auto dl = [&](int c) { return uint64_t((b.q[c] - a.q[c]) & 7); };
        v |= uint64_t(a.q[2]) << 43 | dl(2) << 40 | uint64_t(a.q[1]) << 51 | dl(1) << 48 | uint64_t(a.q[0]) << 59 | dl(0) << 56;
        bestWord = v;
      }
    }
  }
  out = bestWord;
}

void picaEncodeEtc1(const std::vector<uint8_t>& rgba, int w, int h, bool withAlpha, int quality, std::vector<uint8_t>& out) {
  out.assign(picaTextureBytes(withAlpha ? PTF_ETC1A4 : PTF_ETC1, w, h), 0);
  uint8_t* o = out.data();
  for (int ty = 0; ty < h / 8; ty++)
    for (int tx = 0; tx < w / 8; tx++)
      for (int sub = 0; sub < 4; sub++) {
        const int x0 = tx * 8 + (sub & 1) * 4, y0 = ty * 8 + (sub >> 1) * 4;
        uint8_t px[16][3];
        uint64_t alpha = 0;
        for (int y = 0; y < 4; y++)
          for (int x = 0; x < 4; x++) {
            const uint8_t* s = &rgba[(size_t(y0 + y) * w + (x0 + x)) * 4];
            // transparent texels carry no colour information worth fitting; give them the block's mean colour below
            for (int c = 0; c < 3; c++) px[4 * y + x][c] = s[c];
            alpha |= uint64_t(q(s[3], 4)) << (4 * (x * 4 + y));
          }
        if (withAlpha) {   // fit colours on visible texels only
          int n = 0; long sum[3] = {};
          for (int k = 0; k < 16; k++) {
            const int x = k & 3, y = k >> 2;
            if (q(rgba[(size_t(y0 + y) * w + (x0 + x)) * 4 + 3], 4) != 0) { n++; for (int c = 0; c < 3; c++) sum[c] += px[k][c]; }
          }
          if (n > 0 && n < 16)
            for (int k = 0; k < 16; k++) {
              const int x = k & 3, y = k >> 2;
              if (q(rgba[(size_t(y0 + y) * w + (x0 + x)) * 4 + 3], 4) == 0) for (int c = 0; c < 3; c++) px[k][c] = uint8_t(sum[c] / n);
            }
        }
        uint64_t word;
        etc1EncodeBlock(px, word, quality);
        if (withAlpha) { for (int k = 0; k < 8; k++) *o++ = uint8_t(alpha >> (8 * k)); }
        for (int k = 0; k < 8; k++) *o++ = uint8_t(word >> (8 * k));
      }
}

uint64_t g_texProf[8] = {};
uint64_t texProfTicks() {
#ifdef __3DS__
  return svcGetSystemTick();
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// ------------------------------------------------------------------------------------------------ choosing a format
namespace {

int bitsPerTexel(int fmt) {
  switch (fmt) {
    case PTF_I4: case PTF_A4: case PTF_ETC1: return 4;
    case PTF_IA4: case PTF_I8: case PTF_A8: case PTF_ETC1A4: return 8;
    case PTF_RGB5A1: case PTF_RGB565: case PTF_RGBA4: case PTF_IA8: case PTF_HILO8: return 16;
    case PTF_RGB8: return 24;
  }
  return 32;
}

// error of `got` against `ref` in 8-bit units: colour channels always, alpha only when it matters
void measure(const std::vector<uint8_t>& ref, const std::vector<uint8_t>& got, bool alpha, double& mx, double& mean) {
  mx = 0; mean = 0;
  size_t n = 0;
  for (size_t i = 0; i < ref.size(); i += 4)
    for (int c = 0; c < (alpha ? 4 : 3); c++) {
      const double d = std::abs(int(ref[i + size_t(c)]) - int(got[i + size_t(c)]));
      mx = std::max(mx, d);
      mean += d;
      n++;
    }
  mean = n ? mean / double(n) : 0;
}

}  // namespace

namespace {
// Encode compact fitted sources one 8x8 tile at a time. PICA tiles and ETC
// sub-blocks are independent, so concatenation is byte-identical to a full fit.
void encodeInto(const MipImage& m, int fmt, int quality, uint8_t* dst) {
  std::memset(dst,0,picaTextureBytes(fmt,m.w,m.h));
  const size_t tb=tileBytes(fmt);
  const bool etc=fmt==PTF_ETC1 || fmt==PTF_ETC1A4;
  std::vector<uint8_t> tile(etc?256:0), encoded;
  for(int ty=0;ty<m.h;ty+=8) for(int tx=0;tx<m.w;tx+=8,dst+=tb) {
    if(etc) {
      for(int y=0;y<8;++y) for(int x=0;x<8;++x)
        std::memcpy(tile.data()+(y*8+x)*4,m.pixel(tx+x,ty+y),4);
      picaEncodeEtc1(tile,8,8,fmt==PTF_ETC1A4,quality,encoded);
      std::memcpy(dst,encoded.data(),tb);
    } else {
      for(int y=0;y<8;++y) for(int x=0;x<8;++x)
        encodeTexel(fmt,m.pixel(tx+x,ty+y),dst,morton(x,y));
    }
  }
 }
void encodeMapped(const MipImage& m,int fmt,int quality,std::vector<uint8_t>& out){
  out.resize(picaTextureBytes(fmt,m.w,m.h));encodeInto(m,fmt,quality,out.data());
}
}

PicaTexture chooseFormatLevels(const std::vector<MipImage>& L, const TexConvOptions& opt) {
  PicaTexture T;
  T.width = L[0].w; T.height = L[0].h;
  T.levels = int(L.size());

  // does alpha matter and is there any?
  bool alphaVaries = false;
  for (const MipImage& m : L)
    for (int y=0;y<m.h;++y) for(int x=0;x<m.w;++x) if(m.pixel(x,y)[3]!=255) { alphaVaries=true; break; }
  const bool alpha = opt.needAlpha && alphaVaries;

  auto encodeAll = [&](int fmt, bool alphaMatters, bool* allExact, std::vector<uint8_t>& out) {
    out.clear();
    T.levelOffset.clear();
    bool ok = true;
    for (const MipImage& m : L) {
      std::vector<uint8_t> d;
      T.levelOffset.push_back(out.size());
      if (!m.sourceX.empty()) encodeMapped(m,fmt,opt.etc1Quality,d);
      else if (fmt == PTF_ETC1 || fmt == PTF_ETC1A4) picaEncodeEtc1(m.rgba, m.w, m.h, fmt == PTF_ETC1A4, opt.etc1Quality, d);
      else ok = picaEncode(fmt, m.rgba, m.w, m.h, d, alphaMatters, allExact != nullptr) && ok;   // no verify decode when nobody asks
      // Transfer the first encoded level instead of holding two full copies.
      if (out.empty()) out = std::move(d);
      else out.insert(out.end(), d.begin(), d.end());
    }
    if (allExact) *allExact = ok;
  };

  // Only the chosen final format uses external storage; failed candidates never
  // allocate GPU resources. The byte layout and per-tile encoder are unchanged.
  auto finalOutput = [&](int fmt,bool alphaMatters,std::vector<uint8_t>* ready=nullptr) {
    if(opt.output) {
      size_t bytes=0;T.levelOffset.clear();
      for(const auto& m:L){T.levelOffset.push_back(bytes);bytes+=picaTextureBytes(fmt,m.w,m.h);}
      if(uint8_t* dst=opt.output(fmt,T.width,T.height,T.levels,T.levelOffset,bytes)) {
        if(ready) std::memcpy(dst,ready->data(),bytes);
        else for(size_t i=0;i<L.size();++i) encodeInto(L[i],fmt,opt.etc1Quality,dst+T.levelOffset[i]);
        T.externalOutput=true;T.encodedBytes=bytes;return;
      }
    }
    if(ready)T.data=std::move(*ready);else encodeAll(fmt,alphaMatters,nullptr,T.data);
    T.encodedBytes=T.data.size();
  };

  // 1. the smallest PICA format that reproduces the GC texels exactly (in every level)
  constexpr int nOrder = int(sizeof(kExactOrder) / sizeof(kExactOrder[0]));
  // (Claude) One scan ORs every texel's inexactMask (picaEncode's per-texel test: encodeTexel, then picaSampleTexel of that
  // texel; texels are independent), so no format is encoded just to find out that it fails. Same choice, same bytes.
  const uint64_t tPick = texProfTicks();
  uint32_t inexact = 0;
  const uint32_t allSmaller = (1u << (nOrder - 1)) - 1;
  for (const MipImage& m : L) {
    const uint8_t* px = m.rgba.data();
    const size_t n = size_t(m.w) * m.h;
    for (size_t t = 0; t < n && inexact != allSmaller;) {
      const size_t end = std::min(n, t + 64);
      for (; t < end; t++) inexact |= inexactMask(m.sourceX.empty()?px+t*4:m.pixel(int(t%size_t(m.w)),int(t/size_t(m.w))), opt.needAlpha);
    }
  }
  // (Claude) The scan is picaEncode's own test, so the first survivor is the exact format; it is encoded once, and only
  // when no lossy format below replaces it (it used to be encoded before the lossy trial and again after a rejection).
  int exactFmt = PTF_RGBA8;
  for (int i = 0; i < nOrder; i++) if (!(inexact >> i & 1)) { exactFmt = kExactOrder[i]; break; }
  T.fmt = exactFmt;
  T.exact = true;
  const uint64_t tLossy = texProfTicks();
  g_texProf[2] += tLossy - tPick;
  struct LossyClock { uint64_t t; ~LossyClock() { g_texProf[3] += texProfTicks() - t; } } lossyClock{tLossy};

  // 2. an exact result of 24 or 32 bits per texel on a large texture: try the smaller lossy formats
  const size_t texels0 = size_t(L[0].w) * L[0].h;
  if (opt.allowLossy && bitsPerTexel(exactFmt) > 16 && texels0 >= size_t(opt.lossyMinTexels)) {
    std::vector<int> fmts;
    if (alpha) {
      fmts = {PTF_ETC1A4, PTF_RGBA4};
      bool bin = true;
      for (const MipImage& m : L) for(int y=0;y<m.h;++y) for(int x=0;x<m.w;++x) { const uint8_t a=m.pixel(x,y)[3];if(a!=0&&a!=255) {bin=false;break;} }
      if (bin) fmts.insert(fmts.begin() + 1, PTF_RGB5A1);
    } else fmts = {PTF_ETC1, PTF_RGB565};
    if (!opt.tryEtc1) fmts.erase(fmts.begin());   // ETC1 / ETC1A4 lead both lists
    for (int fmt : fmts) {
      std::vector<uint8_t> data;
      const bool etc = fmt == PTF_ETC1 || fmt == PTF_ETC1A4;
      double mx = 0, sum = 0;
      size_t cnt = 0;bool rejected=false;
      if (etc) {
        encodeAll(fmt, alpha, nullptr, data);
        for (size_t k = 0; k < L.size(); k++) {
          const MipImage& m = L[k];
          double lm, lmean;
          if(m.sourceX.empty()) measure(m.rgba, picaDecodeImage(fmt, data.data() + T.levelOffset[k], m.w, m.h), alpha, lm, lmean);
          else {
            uint64_t sum=0;unsigned mx=0;const int nc=alpha?4:3;
            for(int y=0;y<m.h;++y) for(int x=0;x<m.w;++x) {
              uint8_t got[4];picaSampleTexel(fmt,data.data()+T.levelOffset[k],m.w,x,y,got);
              const uint8_t* ref=m.pixel(x,y);
              for(int c=0;c<nc;++c) {unsigned d=unsigned(std::abs(int(ref[c])-int(got[c])));mx=std::max(mx,d);sum+=d;}
            }
            lm=mx;lmean=double(sum)/double(size_t(m.w)*m.h*nc);
          }
          const size_t n = size_t(m.w) * m.h * (alpha ? 4 : 3);
          mx = std::max(mx, lm); sum += lmean * double(n); cnt += n;
        }
      } else {
        // (Claude) the error of the decoded encoding is the per-texel round trip (picaRoundTrip = encode + sample), so it is
        // measured without encoding; same sums in the same order as measure() (integer sums are exact in a double)
        const int nc = alpha ? 4 : 3;
        for (const MipImage& m : L) {
          uint32_t lmx = 0; uint64_t lsum = 0;
          const size_t nt = size_t(m.w) * m.h;
          const uint8_t* px = m.rgba.data();
          if(opt.fastLossyError){
            if(!measureErrorTable(m,fmt,alpha,opt.lossyMaxErrLimit,lmx,lsum)){rejected=true;break;}
          }else for (size_t t = 0; t < nt; t++) {
            uint8_t o[4];
            const uint8_t* src=m.sourceX.empty()?px+t*4:m.pixel(int(t%size_t(m.w)),int(t/size_t(m.w)));
            picaRoundTrip(fmt, src, o);
            for (int c = 0; c < nc; c++) { const uint32_t d = uint32_t(std::abs(int(src[c]) - int(o[c]))); if (d > lmx) lmx = d; lsum += d; }
          }
          const size_t n = nt * size_t(nc);
          const double lmean = n ? double(lsum) / double(n) : 0;
          mx = std::max(mx, double(lmx)); sum += lmean * double(n); cnt += n;
        }
      }
      if(rejected)continue;
      const double mean = cnt ? sum / double(cnt) : 0;
      if (mean <= opt.lossyMeanErrLimit && mx <= opt.lossyMaxErrLimit) {
        T.fmt = fmt;finalOutput(fmt,alpha,etc?&data:nullptr);T.exact = false; T.maxErr255 = mx; T.meanErr255 = mean; g_texProf[6]++;
        T.note = std::string("lossy ") + picaTexFmtName(fmt) + " instead of exact " + picaTexFmtName(exactFmt) + " (max error " + std::to_string(int(mx)) + "/255)";
        return T;
      }
    }
    finalOutput(exactFmt,opt.needAlpha);
    T.note = std::string("no lossy format within the error limits: kept exact ") + picaTexFmtName(exactFmt);
    return T;
  }
  finalOutput(exactFmt,opt.needAlpha);
  return T;
}

PicaTexture chooseFormat(const std::vector<uint8_t>& rgba, int w, int h, const TexConvOptions& opt) {
  // pad to multiples of 8 by repetition
  const int pw = std::max(8, (w + 7) & ~7), ph = std::max(8, (h + 7) & ~7);
  MipImage m;
  m.w = pw; m.h = ph;
  m.rgba.resize(size_t(pw) * ph * 4);
  for (int y = 0; y < ph; y++)
    for (int x = 0; x < pw; x++) std::memcpy(&m.rgba[(size_t(y) * pw + x) * 4], &rgba[(size_t(y % h) * w + (x % w)) * 4], 4);
  std::vector<MipImage> levels;
  levels.push_back(std::move(m));
  PicaTexture T = chooseFormatLevels(levels, opt);
  if (pw != w || ph != h) {
    const bool padExact = (pw % w == 0) && (ph % h == 0);
    T.note = "padded " + std::to_string(w) + "x" + std::to_string(h) + " to " + std::to_string(pw) + "x" + std::to_string(ph) +
             (padExact ? " by repetition (exact for REPEAT wrapping)" : " by repetition (NOT a true tiling of the original: wrong under REPEAT wrapping)") +
             (T.note.empty() ? "" : "; " + T.note);
    if (!padExact) T.exact = false;
  }
  return T;
}

}  // namespace t2p
