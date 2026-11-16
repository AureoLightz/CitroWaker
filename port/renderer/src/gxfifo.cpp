#include "gxfifo.h"
#if defined(__3DS__) || defined(T2P_TEST_MATERIAL_REFS)
extern "C" const t2p::GxStateProgram* n3dsMaterialBorrow(uint64_t);
extern "C" void n3dsMaterialRelease(uint64_t);
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>

thread_local uint64_t g_t2pProf[16] = {};
bool g_gxOriginProf = false; int g_gxOrigin = 0; uint64_t g_gxOriginCount[2][5] = {};
int g_benchRepeat[9] = {};
unsigned long g_deferWhy[8] = {};
uint32_t g_deferMin = 32;   // (Claude) smallest deferred job, in attribute-vertices   // TEMP diagnostic
#ifdef __3DS__
bool g_t2pProfOn = false;
uint32_t g_t2pProfPeriod = 1;
thread_local bool g_t2pProfNow = false;
#endif
namespace t2p {
bool g_decodedTightGrowth=false;
bool g_stateCpIncremental=false;

int g_decodedInitMode=0;
namespace {

inline uint32_t rd32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint32_t rd16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
inline uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

int compBytes(int fmt) {
  switch (fmt) {
    case GXC_U8: case GXC_S8: return 1;
    case GXC_U16: case GXC_S16: return 2;
    default: return 4;
  }
}
int clrBytes(int fmt) {
  switch (fmt) {
    case GXCL_RGB565: case GXCL_RGBA4: return 2;
    case GXCL_RGB8: case GXCL_RGBA6: return 3;
    default: return 4;
  }
}
int idxBytes(int type) { return type == GXA_INDEX16 ? 2 : 1; }

uint8_t ex4(uint32_t n) { return uint8_t((n << 4) | n); }
uint8_t ex5(uint32_t n) { return uint8_t((n << 3) | (n >> 2)); }
uint8_t ex6(uint32_t n) { return uint8_t((n << 2) | (n >> 4)); }

void readColor(const uint8_t* p, int fmt, bool be, uint8_t out[4]) {
  switch (fmt) {
    case GXCL_RGB565: { const uint32_t v = be ? rd16(p) : le16(p); out[0] = ex5(v >> 11 & 31); out[1] = ex6(v >> 5 & 63); out[2] = ex5(v & 31); out[3] = 255; break; }
    case GXCL_RGB8: case GXCL_RGBX8: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
    case GXCL_RGBA4: { const uint32_t v = be ? rd16(p) : le16(p); out[0] = ex4(v >> 12 & 15); out[1] = ex4(v >> 8 & 15); out[2] = ex4(v >> 4 & 15); out[3] = ex4(v & 15); break; }
    case GXCL_RGBA6: { const uint32_t v = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; out[0] = ex6(v >> 18 & 63); out[1] = ex6(v >> 12 & 63); out[2] = ex6(v >> 6 & 63); out[3] = ex6(v & 63); break; }
    default: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; break;
  }
}

// bytes one attribute occupies in the command stream
uint32_t attrStreamBytes(int type, uint32_t directBytes, uint32_t indexCount = 1) {
  if (type == GXA_NONE) return 0;
  if (type == GXA_DIRECT) return directBytes;
  return uint32_t(idxBytes(type)) * indexCount;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------- register decoding
GxVcd GxRegs::vcd() const {
  GxVcd d;
  const uint32_t lo = cp[0x50], hi = cp[0x60];
  d.pmIdx = uint8_t(bits(lo, 0, 1));
  for (int i = 0; i < 8; i++) d.texMtxIdx[i] = uint8_t(bits(lo, 1 + i, 1));
  d.pos = uint8_t(bits(lo, 9, 2));
  d.nrm = uint8_t(bits(lo, 11, 2));
  d.clr[0] = uint8_t(bits(lo, 13, 2));
  d.clr[1] = uint8_t(bits(lo, 15, 2));
  for (int i = 0; i < 8; i++) d.tex[i] = uint8_t(bits(hi, 2 * i, 2));
  return d;
}

GxVat GxRegs::vat(int idx) const {
  GxVat v;
  const uint32_t a = cp[0x70 + idx], b = cp[0x80 + idx], c = cp[0x90 + idx];
  v.posCnt = uint8_t(bits(a, 0, 1));
  v.posFmt = uint8_t(bits(a, 1, 3));
  v.posShift = uint8_t(bits(a, 4, 5));
  v.nrmCnt = uint8_t(bits(a, 9, 1));
  v.nrmFmt = uint8_t(bits(a, 10, 3));
  v.clrCnt[0] = uint8_t(bits(a, 13, 1));
  v.clrFmt[0] = uint8_t(bits(a, 14, 3));
  v.clrCnt[1] = uint8_t(bits(a, 17, 1));
  v.clrFmt[1] = uint8_t(bits(a, 18, 3));
  v.texCnt[0] = uint8_t(bits(a, 21, 1));
  v.texFmt[0] = uint8_t(bits(a, 22, 3));
  v.texShift[0] = uint8_t(bits(a, 25, 5));
  v.nrmIdx3 = bits(a, 31, 1) != 0;
  v.texCnt[1] = uint8_t(bits(b, 0, 1));
  v.texFmt[1] = uint8_t(bits(b, 1, 3));
  v.texShift[1] = uint8_t(bits(b, 4, 5));
  v.texCnt[2] = uint8_t(bits(b, 9, 1));
  v.texFmt[2] = uint8_t(bits(b, 10, 3));
  v.texShift[2] = uint8_t(bits(b, 13, 5));
  v.texCnt[3] = uint8_t(bits(b, 18, 1));
  v.texFmt[3] = uint8_t(bits(b, 19, 3));
  v.texShift[3] = uint8_t(bits(b, 22, 5));
  v.texCnt[4] = uint8_t(bits(b, 27, 1));
  v.texFmt[4] = uint8_t(bits(b, 28, 3));
  v.texShift[4] = uint8_t(bits(c, 0, 5));
  v.texCnt[5] = uint8_t(bits(c, 5, 1));
  v.texFmt[5] = uint8_t(bits(c, 6, 3));
  v.texShift[5] = uint8_t(bits(c, 9, 5));
  v.texCnt[6] = uint8_t(bits(c, 14, 1));
  v.texFmt[6] = uint8_t(bits(c, 15, 3));
  v.texShift[6] = uint8_t(bits(c, 18, 5));
  v.texCnt[7] = uint8_t(bits(c, 23, 1));
  v.texFmt[7] = uint8_t(bits(c, 24, 3));
  v.texShift[7] = uint8_t(bits(c, 27, 5));
  return v;
}

float GxRegs::xfFloat(uint32_t addr) const {
  float f;
  const uint32_t u = addr < 0x1100 ? xf[addr] : 0;
  std::memcpy(&f, &u, 4);
  return f;
}

uint32_t gxVertexSize(const GxVcd& d, const GxVat& v) {
  uint32_t n = d.pmIdx ? 1 : 0;
  for (int i = 0; i < 8; i++) n += d.texMtxIdx[i] ? 1 : 0;
  n += attrStreamBytes(d.pos, uint32_t(v.posCnt ? 3 : 2) * compBytes(v.posFmt));
  n += attrStreamBytes(d.nrm, uint32_t(v.nrmCnt ? 9 : 3) * compBytes(v.nrmFmt), (v.nrmCnt && v.nrmIdx3) ? 3 : 1);
  for (int c = 0; c < 2; c++) n += attrStreamBytes(d.clr[c], clrBytes(v.clrFmt[c]));
  for (int t = 0; t < 8; t++) n += attrStreamBytes(d.tex[t], uint32_t(v.texCnt[t] ? 2 : 1) * compBytes(v.texFmt[t]));
  return n;
}

// ---------------------------------------------------------------------------------------------------- stream processing
void GxFifo::handleBp(uint32_t v) { handleBpPrepared(v,stateDirtyBp(v>>24)); }
void GxFifo::handleBpPrepared(uint32_t v,uint32_t dirty) {
  const uint32_t id = v >> 24;
  if (id == 0xFE) {              // BP mask: applies to the next BP write only
    bpMask_ = v & 0xFFFFFF;
    return;
  }
  const uint32_t mask = bpMask_;
  bpMask_ = 0xFFFFFF;
  // Aurora palettes are bound by LOAD_TEXOBJ/LOAD_TLUT logical slots. J3D
  // material lists subsequently write console TMEM regions (e.g. 896 + 16*m),
  // which are not Aurora slots. Match Aurora's BP handler: ignore SETTLUT;
  // metadata's bpSet still updates the binding and palette format below.
  if (aurora_ && ((id >= 0x98 && id <= 0x9B) || (id >= 0xB8 && id <= 0xBB))) return;
  const uint32_t old = regs_.bp[id];
  setReg(regs_.bp[id], (id << 24) | ((old & ~mask) & 0xFFFFFF) | (v & mask & 0xFFFFFF), dirty);
  if (!regs_.bpWritten[id]) { regs_.bpWritten[id] = true; touchStatic();stateGen_++; stateDirty_ |= dirty; }
  if (id >= 0xE0 && id <= 0xE7) {   // the TEV colour / konstant colour banks share these ids
    const int bank = (v >> 23) & 1;
    uint32_t& r = regs_.tevRaw[bank][id - 0xE0];
    setReg(r, ((r & ~mask) | (v & mask)) & 0xFFFFFF, kStateDirtyTevReg);
  }
  if (sink_) {
    if (id == 0x52) {
      if (aurora_ && ((regs_.bp[0x52] >> 14) & 1)) {   // display copy (the capture adds them; Aurora's GXCopyDisp writes nothing): not the texture copy's destination
        const uint32_t dst = regs_.copyDstAddr;
        regs_.copyDstAddr = 0;
        sink_->onCopy(regs_);
        regs_.copyDstAddr = dst;
      } else if (aurora_) {   // Aurora's GXCopyTex writes only clear / not-XFB into its shadow of this register: add what GXSetTexCopyDst sets on the console
        const uint32_t mask = (1u << 3) | (7u << 4) | (1u << 9) | (3u << 15);
        regs_.bp[0x52] = (regs_.bp[0x52] & ~mask) | auroraCopyBits_;
        const uint32_t pe = regs_.bp[0x43];
        if (auroraCopyZ_) regs_.bp[0x43] = (pe & ~7u) | 3u;   // the console's GXCopyTex switches the PE to Z24 around a Z copy
        sink_->onCopy(regs_);
        regs_.bp[0x43] = pe;
      } else {
        sink_->onCopy(regs_);
      }
    }
    else if (id == 0x65 && !aurora_) sink_->onTlutLoad(regs_);   // Aurora: GX_AURORA_LOAD_TLUT carries the source and completes the load
  }
}

void GxFifo::handleCp(uint8_t addr, uint32_t v) {
  if (addr >= 0xA0 && addr < 0xC0) regs_.cp[addr] = v;   // array base / stride: only used to fetch vertices, which a draw has already decoded
  else {
    // Changing vertex format must still break batches and refresh their format,
    // but does not invalidate unchanged BP/XF material, light or texture state.
    // The decoder always recalculates pose count and selected matrix per draw.
    const bool format=addr==0x50||addr==0x60||(addr>=0x70&&addr<=0x77)||(addr>=0x80&&addr<=0x87)||(addr>=0x90&&addr<=0x97);
    setReg(regs_.cp[addr],v,g_stateCpIncremental&&format?kStateDirtyVertexFormat:kStateDirtyOther);
  }
}

void GxFifo::applyProgramXf(uint32_t addr,const uint32_t* words,uint32_t n) {
  for(uint32_t i=0;i<n;++i)if(addr+i<0x1100)setReg(regs_.xf[addr+i],words[i],stateDirtyXf(addr+i));
}

void GxFifo::handleXf(uint32_t addr, const uint8_t* words, uint32_t n) {
  if (addr + n <= 0x600) {   // a matrix load (the common case): one group for the whole run
    for (uint32_t i = 0; i < n; i++) setReg(regs_.xf[addr + i], rd32(words + 4 * i), kStateDirtyXfMtx);
    return;
  }
  for (uint32_t i = 0; i < n; i++)
    if (addr + i < 0x1100) setReg(regs_.xf[addr + i], rd32(words + 4 * i), stateDirtyXf(addr + i));
}

const uint8_t* GxFifo::guestElement(int arrayId, uint32_t index, uint32_t need) const {
  if (!guestRead_) return nullptr;
  const uint32_t base = regs_.cp[0xA0 + arrayId] & (aurora_ ? 0xFFFFFFFFu : 0x03FFFFFFu), stride = regs_.cp[0xB0 + arrayId] & 0xFF;
  return guestRead_(base + index * stride, need);
}

void GxFifo::indexedXf(int arrayId, uint16_t index, uint16_t addrLen) {
  const uint32_t len = (addrLen >> 12) + 1, dst = addrLen & 0x0FFF;
  const GxArray& a = arrays_[arrayId];
  const uint64_t off = uint64_t(index) * (a.strideFromCp ? (regs_.cp[0xB0 + arrayId] & 0xFF) : a.stride);
  if (!a.data) {
    const uint8_t* g = guestElement(arrayId, index, len * 4);
    if (!g) { err("indexed XF load outside guest memory (array " + std::to_string(arrayId) + ", index " + std::to_string(index) + ")"); return; }
    for (uint32_t i = 0; i < len; i++) if (dst + i < 0x1100) setReg(regs_.xf[dst + i], rd32(g + 4 * i), stateDirtyXf(dst + i));
    return;
  }
  if (off + uint64_t(len) * 4 > a.size) { err("indexed XF load outside its array (array " + std::to_string(arrayId) + ", index " + std::to_string(index) + ")"); return; }
  for (uint32_t i = 0; i < len; i++)
    if (dst + i < 0x1100) setReg(regs_.xf[dst + i], a.bigEndian ? rd32(a.data + off + 4 * i) : le32(a.data + off + 4 * i), stateDirtyXf(dst + i));
}

GxFifo::FmtCache& GxFifo::vertexFormat(int vatIdx) {
  FmtCache& c = fmt_[vatIdx];
  const uint32_t raw[5] = {regs_.cp[0x50], regs_.cp[0x60], regs_.cp[0x70 + vatIdx], regs_.cp[0x80 + vatIdx], regs_.cp[0x90 + vatIdx]};
  if (!c.valid || std::memcmp(raw, c.raw, sizeof raw) != 0) {
    std::memcpy(c.raw, raw, sizeof raw);
    c.vcd = regs_.vcd();
    c.vat = regs_.vat(vatIdx);
    c.vsize = gxVertexSize(c.vcd, c.vat);
    c.valid = true;
    c.planned = false;
  }
  return c;
}

namespace {
// One attribute of the vertex format, resolved for one draw: where its bytes come from (the stream, or an array element) and where they go.
using AttrPlan = GxAttrPlan;

inline float readOne(const uint8_t* p, int fmt, bool be) {
  switch (fmt) {
    case GXC_U8: return float(p[0]);
    case GXC_S8: return float(int8_t(p[0]));
    case GXC_U16: return float(be ? rd16(p) : le16(p));
    case GXC_S16: return float(int16_t(be ? rd16(p) : le16(p)));
    default: { const uint32_t u = be ? rd32(p) : le32(p); float f; std::memcpy(&f, &u, 4); return f; }
  }
}
template <int FMT, bool BE> inline float readT(const uint8_t* p) {
  if constexpr (FMT == GXC_U8) return float(p[0]);
  else if constexpr (FMT == GXC_S8) return float(int8_t(p[0]));
  else if constexpr (FMT == GXC_U16) return float(BE ? rd16(p) : le16(p));
  else if constexpr (FMT == GXC_S16) return float(int16_t(BE ? rd16(p) : le16(p)));
  else { const uint32_t u = BE ? rd32(p) : le32(p); float f; std::memcpy(&f, &u, 4); return f; }
}
constexpr int compBytesT(int fmt) { return fmt == GXC_U8 || fmt == GXC_S8 ? 1 : fmt == GXC_U16 || fmt == GXC_S16 ? 2 : 4; }

// Where vertex v's element of an attribute is: the stream (direct) or a registered array (indexed). nullptr: outside the array (the caller zeroes it).
// Only for attributes with a registered array or direct data (AttrPlan::base set, or GXA_DIRECT); guest-memory arrays take the generic path.
inline const uint8_t* elementAt(const AttrPlan& a, const uint8_t* s) {
  if (a.type == GXA_DIRECT) return s;
  const uint32_t idx = a.type == GXA_INDEX16 ? rd16(s) : *s;
  const uint64_t off = uint64_t(idx) * a.stride;
  return off + a.need > a.size ? nullptr : a.base + off;
}

// One float attribute (position / normal / texcoord) for all n vertices: the format is a template parameter, so the loop has no per-component switch.
// Writes outN floats per vertex: the nc components, the rest 0. Returns false when an index fell outside its array.
template <int FMT, bool BE, int TYPE = -1>
bool floatAttr(const AttrPlan& a, const uint8_t* v0, uint32_t vsize, uint32_t n, float* o, int outN) {
  constexpr int CB = compBytesT(FMT);
  bool ok = true;
  const int nc = a.nc;
  const float scale = a.scale;
  for (uint32_t v = 0; v < n; v++, o += outN) {
    const uint8_t* src;
    const uint8_t* stream = v0 + size_t(v) * vsize + a.offset;
    if constexpr (TYPE == GXA_DIRECT) src = stream;
    else if constexpr (TYPE == GXA_INDEX8 || TYPE == GXA_INDEX16) {
      // The caller proved stride and need <=65535, so even INDEX16's
      // largest index cannot overflow this 32-bit offset or its bound check.
      const uint32_t idx = TYPE == GXA_INDEX16 ? rd16(stream) : *stream;
      const uint32_t off = idx * a.stride;
      src = off + a.need > a.size ? nullptr : a.base + off;
    } else src = elementAt(a, stream);
    if (!src) { ok = false; for (int c = 0; c < outN; c++) o[c] = 0; continue; }
    o[0] = readT<FMT, BE>(src) * scale;
    if (nc > 1) o[1] = readT<FMT, BE>(src + CB) * scale; else o[1] = 0;
    if (outN == 3) o[2] = nc > 2 ? readT<FMT, BE>(src + 2 * CB) * scale : 0;
  }
  return ok;
}
// (Claude v78) floatAttr with the component counts fixed too (NC read, OUTN written: 3/3 or 2/2) and every plan field held in a local, so the
// per-vertex loop has no count branches and no reloads through the plan. Same reads, same float operations (value * scale), same zeroing on a bad index.
template <int FMT, bool BE, int TYPE, int NC>
bool floatAttrN(const AttrPlan& a, const uint8_t* v0, uint32_t vsize, uint32_t n, float* __restrict o) {
  constexpr int CB = compBytesT(FMT);
  const float scale = a.scale;
  const uint8_t* __restrict stream = v0 + a.offset;
  const uint8_t* const base = a.base;
  const uint32_t stride = a.stride, limit = a.size, need = a.need;
  bool ok = true;
  for (uint32_t v = 0; v < n; v++, o += NC, stream += vsize) {
    const uint8_t* src;
    if constexpr (TYPE == GXA_DIRECT) src = stream;
    else {
      const uint32_t idx = TYPE == GXA_INDEX16 ? rd16(stream) : *stream;
      const uint32_t off = idx * stride;   // (the caller proved stride, need <= 65535: no overflow)
      if (off + need > limit) { ok = false; for (int c = 0; c < NC; c++) o[c] = 0; continue; }
      src = base + off;
    }
    const float x = readT<FMT, BE>(src) * scale;
    const float y = readT<FMT, BE>(src + CB) * scale;
    if constexpr (NC == 3) { const float z = readT<FMT, BE>(src + 2 * CB) * scale; o[0] = x; o[1] = y; o[2] = z; }
    else { o[0] = x; o[1] = y; }
  }
  return ok;
}
template <bool BE, int TYPE, int NC>
bool floatAttrNFmt(const AttrPlan& a, const uint8_t* v0, uint32_t vsize, uint32_t n, float* o) {
  switch (a.fmt) {
    case GXC_U8: return floatAttrN<GXC_U8, BE, TYPE, NC>(a, v0, vsize, n, o);
    case GXC_S8: return floatAttrN<GXC_S8, BE, TYPE, NC>(a, v0, vsize, n, o);
    case GXC_U16: return floatAttrN<GXC_U16, BE, TYPE, NC>(a, v0, vsize, n, o);
    case GXC_S16: return floatAttrN<GXC_S16, BE, TYPE, NC>(a, v0, vsize, n, o);
    default: return floatAttrN<GXC_F32, BE, TYPE, NC>(a, v0, vsize, n, o);
  }
}
template <int TYPE, int NC>
bool floatAttrNType(const AttrPlan& a, bool be, const uint8_t* v0, uint32_t vsize, uint32_t n, float* o) {
  return be ? floatAttrNFmt<true, TYPE, NC>(a, v0, vsize, n, o) : floatAttrNFmt<false, TYPE, NC>(a, v0, vsize, n, o);
}
template <int NC>
bool floatAttrNAny(const AttrPlan& a, bool be, const uint8_t* v0, uint32_t vsize, uint32_t n, float* o) {
  switch (a.type) {
    case GXA_DIRECT: return floatAttrNType<GXA_DIRECT, NC>(a, be, v0, vsize, n, o);
    case GXA_INDEX8: return floatAttrNType<GXA_INDEX8, NC>(a, be, v0, vsize, n, o);
    default: return floatAttrNType<GXA_INDEX16, NC>(a, be, v0, vsize, n, o);
  }
}
// Colours: RGBA8 / RGB8 / RGBX8 (byte copies) specialised by index type; other formats keep readColor.
template <int TYPE, int CF>
bool colorAttrN(const AttrPlan& a, const uint8_t* v0, uint32_t vsize, uint32_t n, uint8_t* __restrict o) {
  const uint8_t* __restrict stream = v0 + a.offset;
  const uint8_t* const base = a.base;
  const uint32_t stride = a.stride, limit = a.size, need = a.need;
  bool ok = true;
  for (uint32_t v = 0; v < n; v++, o += 4, stream += vsize) {
    const uint8_t* src;
    if constexpr (TYPE == GXA_DIRECT) src = stream;
    else {
      const uint32_t idx = TYPE == GXA_INDEX16 ? rd16(stream) : *stream;
      const uint32_t off = idx * stride;
      if (off + need > limit) { ok = false; o[0] = o[1] = o[2] = 0; o[3] = 255; continue; }
      src = base + off;
    }
    o[0] = src[0]; o[1] = src[1]; o[2] = src[2]; o[3] = CF == GXCL_RGBA8 ? src[3] : 255;
  }
  return ok;
}
template <int CF>
bool colorAttrNAny(const AttrPlan& a, const uint8_t* v0, uint32_t vsize, uint32_t n, uint8_t* o) {
  switch (a.type) {
    case GXA_DIRECT: return colorAttrN<GXA_DIRECT, CF>(a, v0, vsize, n, o);
    case GXA_INDEX8: return colorAttrN<GXA_INDEX8, CF>(a, v0, vsize, n, o);
    default: return colorAttrN<GXA_INDEX16, CF>(a, v0, vsize, n, o);
  }
}
template <bool BE, int TYPE = -1>
bool floatAttrFmt(const AttrPlan& a, const uint8_t* v0, uint32_t vsize, uint32_t n, float* o, int outN) {
  switch (a.fmt) {
    case GXC_U8: return floatAttr<GXC_U8, BE, TYPE>(a, v0, vsize, n, o, outN);
    case GXC_S8: return floatAttr<GXC_S8, BE, TYPE>(a, v0, vsize, n, o, outN);
    case GXC_U16: return floatAttr<GXC_U16, BE, TYPE>(a, v0, vsize, n, o, outN);
    case GXC_S16: return floatAttr<GXC_S16, BE, TYPE>(a, v0, vsize, n, o, outN);
    default: return floatAttr<GXC_F32, BE, TYPE>(a, v0, vsize, n, o, outN);
  }
}
}  // namespace

// Aurora emits many tiny adjacent primitives. Decode their shared attribute layout
// once, but retain each primitive boundary when constructing triangle indices.
// Opt-in sinks supply their remaining batch capacity; state commands end a run.
int GxFifo::tryDrawRun(const uint8_t*& p,const uint8_t* end) {
  if (!sink_) return 0;
  const uint32_t limit=std::min<uint32_t>(512,sink_->primitiveRunLimit());
  if(limit<6) return 0;
  auto peek=[&](const uint8_t* q,int expectedVat,uint8_t& cmd,const uint8_t*& vertices,uint32_t& count,const uint8_t*& next)->bool {
    if(q>=end) return false;
    uint32_t length=0;bool sized=false;
    if(*q>=0x80) {if(end-q<3)return false;cmd=q[0];count=rd16(q+1);vertices=q+3;}
    else if(aurora_&&*q==0x50&&end-q>=8&&rd16(q+1)==0x40){cmd=q[3];length=rd32(q+4);vertices=q+8;sized=true;}
    else return false;
    const uint8_t prim=cmd&0xf8;
    if(!(prim==GXP_QUADS||prim==GXP_QUADS2||prim==GXP_TRIANGLES||prim==GXP_TRISTRIP||prim==GXP_TRIFAN))return false;
    if(expectedVat>=0&&(cmd&7)!=expectedVat)return false;
    const uint32_t vs=vertexFormat(cmd&7).vsize;
    if(!vs)return false;
    if(sized){if(length%vs)return false;count=length/vs;}
    if(count<(prim==GXP_QUADS||prim==GXP_QUADS2?4u:3u))return false;
    const uint64_t bytes=uint64_t(vs)*count;
    if(bytes>uint64_t(end-vertices))return false;
    next=vertices+bytes;return true;
  };
  const uint8_t* q=p;uint32_t total=0;int parts=0;uint8_t first=0;
  while(parts<int(runParts_.size())) {
    uint8_t cmd;uint32_t count;const uint8_t* vertices;const uint8_t* next;
    if(!peek(q,parts?int(first&7):-1,cmd,vertices,count,next)||count>limit-total)break;
    if(!parts)first=cmd;
    runParts_[parts++]={vertices,count,uint8_t(cmd&0xf8)};total+=count;q=next;
  }
  if(parts<2)return 0;
  const uint64_t rm0=t2pProfTicks();
  // (Claude v79) No consumer of the contiguous stream (decode cache, retention offers, input observer, deferred jobs, bench repeats): the parts are
  // decoded where they lie (handleDraw falls back to the copy below when the fast attribute decode does not apply).
  if(false){   // (Claude v79) in-place part decode measured slower (per-part attribute dispatch on ~5-vertex parts costs more than the copy): off
    if(t2pProfEnabled())g_t2pProf[6]+=t2pProfTicks()-rm0;
    splitRun_=true;const uint8_t* data=runParts_[0].data;
    const bool ok=handleDraw(first,data,nullptr,int(total),runParts_.data(),parts);
    splitRun_=false;p=q;return ok?1:-1;
  }
  const uint32_t vs=vertexFormat(first&7).vsize;
  runBytes_.resize(size_t(total)*vs);size_t at=0;
  for(int i=0;i<parts;i++){size_t bytes=size_t(runParts_[i].count)*vs;std::memcpy(runBytes_.data()+at,runParts_[i].data,bytes);at+=bytes;}
  const uint8_t* data=runBytes_.data();
  if(t2pProfEnabled())g_t2pProf[6]+=t2pProfTicks()-rm0;
  const bool ok=handleDraw(first,data,data+runBytes_.size(),int(total),runParts_.data(),parts);
  p=q;return ok?1:-1;
}

// (Claude) The fast attribute decode of one draw (every attribute at a fixed offset in every vertex), shared by the inline path and the
// deferred jobs. false: an indexed element lay outside its array (zeroed, as before).
static bool decodeAttrsFast(const AttrPlan* A, int nA, const uint8_t* v0, uint32_t vsize, uint32_t nNew, bool fastAttr) {
  bool allOk = true;
  for (int k = 0; k < nA; k++) {
    const AttrPlan& a = A[k];
    switch (a.kind) {
      case AttrPlan::Byte:
        for (uint32_t v = 0; v < nNew; v++) a.out[v] = v0[size_t(v) * vsize + a.offset];
        break;
      case AttrPlan::Clr: {
        uint8_t* o = a.out;
        if (fastAttr && (a.type == GXA_DIRECT || (a.stride <= 65535 && a.need <= 65535)) && (a.fmt == GXCL_RGBA8 || a.fmt == GXCL_RGB8 || a.fmt == GXCL_RGBX8)) {
          if (!(a.fmt == GXCL_RGBA8 ? colorAttrNAny<GXCL_RGBA8>(a, v0, vsize, nNew, o) : colorAttrNAny<GXCL_RGB8>(a, v0, vsize, nNew, o))) allOk = false;
          break;
        }
        for (uint32_t v = 0; v < nNew; v++, o += 4) {
          const uint8_t* src = elementAt(a, v0 + size_t(v) * vsize + a.offset);
          if (src) readColor(src, a.fmt, a.type == GXA_DIRECT ? true : a.be, o);
          else { allOk = false; o[0] = o[1] = o[2] = 0; o[3] = 255; }
        }
        break;
      }
      default: {
        const bool be = a.type == GXA_DIRECT ? true : a.be;
        const int outN = a.kind == AttrPlan::Tex ? 2 : 3;
        float* o = reinterpret_cast<float*>(a.out);
        bool ok;
        const bool small = a.type == GXA_DIRECT || (a.stride <= 65535 && a.need <= 65535);
        if (fastAttr && small && a.nc == outN) ok = outN == 3 ? floatAttrNAny<3>(a, be, v0, vsize, nNew, o) : floatAttrNAny<2>(a, be, v0, vsize, nNew, o);
        else if (fastAttr && small) {
          // Choose direct / INDEX8 / INDEX16 once per attribute, rather than
          // branching and doing a 64-bit multiply for every decoded vertex.
          switch (a.type) {
            case GXA_DIRECT: ok = be ? floatAttrFmt<true,GXA_DIRECT>(a,v0,vsize,nNew,o,outN) : floatAttrFmt<false,GXA_DIRECT>(a,v0,vsize,nNew,o,outN); break;
            case GXA_INDEX8: ok = be ? floatAttrFmt<true,GXA_INDEX8>(a,v0,vsize,nNew,o,outN) : floatAttrFmt<false,GXA_INDEX8>(a,v0,vsize,nNew,o,outN); break;
            default: ok = be ? floatAttrFmt<true,GXA_INDEX16>(a,v0,vsize,nNew,o,outN) : floatAttrFmt<false,GXA_INDEX16>(a,v0,vsize,nNew,o,outN); break;
          }
        } else ok = be ? floatAttrFmt<true>(a,v0,vsize,nNew,o,outN) : floatAttrFmt<false>(a,v0,vsize,nNew,o,outN);
        if (!ok) allOk = false;
        break;
      }
    }
  }
  return allOk;
}

// Jobs are numbered for ever (wrapping 32-bit counters, compared by difference) and live in slot number % kJobs; a slot is rewritten only
// after its previous job is done (published - done < kJobs). The stream copies sit in a linear arena that is rewound when no job is pending.
struct GxFifo::DecodeQueue {
  static constexpr uint32_t kJobs = 256;
  static constexpr size_t kArena = 192u << 10;
  struct Job { AttrPlan attrs[12]; int nA; const uint8_t* v0; uint32_t vsize, n; bool fastAttr; };
  Job jobs[kJobs];
  std::vector<uint8_t> arena = std::vector<uint8_t>(kArena);
  size_t arenaUsed = 0;
  std::atomic<uint32_t> published{0}, next{0}, done{0};
  std::atomic<bool> arrayError{false};
  void (*wake)() = nullptr;
  bool pending() const { return done.load(std::memory_order_acquire) != published.load(std::memory_order_relaxed); }
};
void GxFifo::DecodeQueueDelete::operator()(DecodeQueue* q) const { delete q; }
void GxFifo::setDeferredDecode(bool on, void (*wake)()) {
  if (deferQ_) finishDecodes();
  if (on) { if (!deferQ_) deferQ_.reset(new DecodeQueue); deferQ_->wake = wake; }
  else deferQ_.reset();   // (the helper must have stopped calling runDecodeJob before the queue is switched off)
}
bool GxFifo::runDecodeJob() {
  DecodeQueue* q = deferQ_.get();
  if (!q) return false;
  uint32_t i = q->next.load();
  for (;;) {
    if (int32_t(q->published.load() - i) <= 0) return false;
    if (q->next.compare_exchange_weak(i, i + 1)) break;
  }
  const DecodeQueue::Job& j = q->jobs[i % DecodeQueue::kJobs];
  if (!decodeAttrsFast(j.attrs, j.nA, j.v0, j.vsize, j.n, j.fastAttr)) q->arrayError.store(true);
  q->done.fetch_add(1);
  return true;
}
void GxFifo::finishDecodes() {
  DecodeQueue* q = deferQ_.get();
  if (!q) return;
  if (q->done.load() != q->published.load()) deferredDrains_++;
  while (runDecodeJob()) { deferredSelf_++; g_deferWhy[1]++; }
  while (q->done.load() != q->published.load()) {}   // a job the helper is in the middle of
  q->arenaUsed = 0;
  if (q->arrayError.exchange(false)) err("draw reads an array element outside the arrays the host registered");
}

bool GxFifo::handleDraw(uint8_t cmd, const uint8_t*& p, const uint8_t* end, int count, const PrimitiveRunPart* parts, int partCount) {
  if (count < 0 && end - p < 2) return false;
#ifdef __3DS__
  struct ProfSample { ProfSample() { if (g_t2pProfOn) { static thread_local uint32_t n = 0; g_t2pProfNow = ++n >= g_t2pProfPeriod; if (g_t2pProfNow) n = 0; } } ~ProfSample() { g_t2pProfNow = false; } } profSample;   // (Claude v94)
#endif
  const uint64_t prof0 = t2pProfTicks();
  // one scratch draw, reused: its vectors keep their capacity, so decoding a draw allocates nothing once they have grown (a frame has ~17,000 draws;
  // allocations are slow on the 3DS). The sink gets a const reference valid during onDraw only, as before.
  // the header: what a sink needs to choose where the vertices go
  GxDraw& H = scratch_;
  const uint8_t prim = cmd & 0xF8;
  H.prim = partCount>1 ? GXP_TRIANGLES : prim;
  H.commandCount = partCount>0 ? uint32_t(partCount) : 1;
  H.vatIdx = cmd & 7;
  if (count < 0) { H.vertexCount = rd16(p); p += 2; }
  else H.vertexCount = uint32_t(count);
  FmtCache& F = vertexFormat(H.vatIdx);
  H.vcd = F.vcd;
  H.vat = F.vat;
  H.unsupportedPrim = !(prim == GXP_QUADS || prim == GXP_QUADS2 || prim == GXP_TRIANGLES || prim == GXP_TRISTRIP || prim == GXP_TRIFAN);
  const uint32_t nNew = H.vertexCount;
  const GxVcd& vcd = F.vcd;
  const GxVat& vat = F.vat;
  const uint32_t vsize = F.vsize;
  if (!splitRun_ && uint64_t(end - p) < uint64_t(vsize) * nNew) { p = end; err("draw command is truncated"); return false; }   // (split run: every part was bounds-checked by tryDrawRun)

  const uint64_t pa0 = t2pProfTicks();
  GxDraw* target = sink_ ? sink_->appendTarget(regs_, H) : nullptr;
  if (t2pProfEnabled()) { const uint64_t t = t2pProfTicks(); g_t2pProf[5] += t - pa0; g_t2pProf[7] += t2pProfTicks() - t; }
  if (!target) {   // the scratch draw: emptied, its vectors keep their capacity
    GxVertices& W = H.v;
    W.count = 0;
    W.pos.clear(); W.nrm.clear(); W.pmIdx.clear();
    for (int c = 0; c < 2; c++) W.clr[c].clear();
    for (int t = 0; t < 8; t++) { W.tex[t].clear(); W.texMtxIdx[t].clear(); }
    H.tris.clear();
  }
  GxDraw& D = target ? *target : H;
  GxVertices& V = D.v;
  uint32_t base = D.v.count;   // (re-read after a retention offer: a diverging retained batch is materialized there)

  const uint64_t attrProfileStart=t2pProfTicks();
  // ---- the attributes in stream order, resolved once for the draw (outputs are pointed into the vectors once they are sized, below)
  AttrPlan* A = F.attrs;
  int nA = F.nAttrs;
  const bool refreshPlan = F.planned && cachedVertexPlan_;
  if (!refreshPlan) {
    nA = 0;
    auto addArray = [&](AttrPlan& a, int arrayId) {
      a.arrayId = arrayId;
      a.base = nullptr; a.stride = 0; a.size = 0; a.be = true;
      if (a.type == GXA_DIRECT || a.type == GXA_NONE) return;
      const GxArray& arr = arrays_[arrayId];
      a.be = arr.bigEndian;
      if (arr.data) { a.base = arr.data; a.stride = arr.strideFromCp ? (regs_.cp[0xB0 + arrayId] & 0xFF) : arr.stride; a.size = arr.size; }
    };
    if (vcd.pmIdx) { AttrPlan& a = A[nA++]; a.kind = AttrPlan::Byte; a.slot = 0; a.type = GXA_DIRECT; a.streamBytes = 1; a.out = nullptr; a.outStride = 1; }
    for (int t = 0; t < 8; t++)
      if (vcd.texMtxIdx[t]) { AttrPlan& a = A[nA++]; a.kind = AttrPlan::Byte; a.slot = t + 1; a.type = GXA_DIRECT; a.streamBytes = 1; a.out = nullptr; a.outStride = 1; }
    if (vcd.pos != GXA_NONE) {
      AttrPlan& a = A[nA++];
      a.kind = AttrPlan::Pos; a.type = vcd.pos; a.fmt = vat.posFmt; a.nc = vat.posCnt ? 3 : 2;
      a.need = uint32_t(a.nc) * compBytes(a.fmt);
      a.streamBytes = attrStreamBytes(a.type, a.need);
      a.scale = (a.fmt == GXC_F32 || vat.posShift == 0) ? 1.0f : 1.0f / float(1u << vat.posShift);
      a.out = nullptr; a.outStride = 12;
      addArray(a, GXAR_POS);
    }
    if (vcd.nrm != GXA_NONE) {
      AttrPlan& a = A[nA++];
      const int shift = vat.nrmFmt == GXC_S8 ? 6 : (vat.nrmFmt == GXC_S16 ? 14 : 0);
      a.kind = AttrPlan::Nrm; a.type = vcd.nrm; a.fmt = vat.nrmFmt; a.nc = 3;
      a.need = 3u * compBytes(a.fmt);
      a.streamBytes = attrStreamBytes(a.type, uint32_t(vat.nrmCnt ? 9 : 3) * compBytes(a.fmt), (vat.nrmCnt && vat.nrmIdx3) ? 3 : 1);
      a.scale = shift ? 1.0f / float(1u << shift) : 1.0f;
      a.out = nullptr; a.outStride = 12;
      addArray(a, GXAR_NRM);
    }
    for (int c = 0; c < 2; c++) {
      if (vcd.clr[c] == GXA_NONE) continue;
      AttrPlan& a = A[nA++];
      a.kind = AttrPlan::Clr; a.slot = c; a.type = vcd.clr[c]; a.fmt = vat.clrFmt[c]; a.nc = 4;
      a.need = uint32_t(clrBytes(a.fmt));
      a.streamBytes = attrStreamBytes(a.type, a.need);
      a.scale = 1.0f;
      a.out = nullptr; a.outStride = 4;
      addArray(a, GXAR_CLR0 + c);
    }
    for (int t = 0; t < 8; t++) {
      if (vcd.tex[t] == GXA_NONE) continue;
      AttrPlan& a = A[nA++];
      a.kind = AttrPlan::Tex; a.slot = t; a.type = vcd.tex[t]; a.fmt = vat.texFmt[t]; a.nc = vat.texCnt[t] ? 2 : 1;
      a.need = uint32_t(a.nc) * compBytes(a.fmt);
      a.streamBytes = attrStreamBytes(a.type, a.need);
      a.scale = (a.fmt == GXC_F32 || vat.texShift[t] == 0) ? 1.0f : 1.0f / float(1u << vat.texShift[t]);
      a.out = nullptr; a.outStride = 8;
      addArray(a, GXAR_TEX0 + t);
    }


    F.nAttrs = nA;
    F.planned = true;
  }
  // The recipe depends only on the VAT/VCD words checked by vertexFormat.
  // Never cache source bindings or destination addresses: skinning and array
  // rebinding may change them between consecutive draws with the same format.
  if (refreshPlan) for (int k = 0; k < nA; ++k) {
    AttrPlan& a = A[k];
    if (a.kind != AttrPlan::Byte && a.type != GXA_DIRECT && a.type != GXA_NONE) {
      const GxArray& arr = arrays_[a.arrayId];
      a.be = arr.bigEndian;
      a.base = arr.data;
      a.stride = arr.strideFromCp ? (regs_.cp[0xB0+a.arrayId]&0xff) : arr.stride;
      a.size = arr.size;
    }
  }
  bool arrayError = false;
  static const bool dbg = std::getenv("GXFIFO_ARRAYDBG") != nullptr;
  uint32_t off = 0;
  bool fast = !dbg;   // attribute by attribute (each at a fixed offset in every vertex), unless an indexed array lives in guest memory
  for (int k = 0; k < nA; k++) {
    A[k].offset = off;
    off += A[k].streamBytes;
    if (A[k].kind != AttrPlan::Byte && A[k].type != GXA_DIRECT && !A[k].base) fast = false;
  }
  if (off != vsize) fast = false;   // (cannot happen: gxVertexSize sums the same sizes)
  if(t2pProfEnabled())g_t2pProf[14]+=t2pProfTicks()-attrProfileStart;
  GxDrawInputs in;
  const bool offer = retainOffers_ && target && !H.unsupportedPrim && !decodeCache_;
  if (inputObserver_ || offer) {
    in.cmd = cmd; in.partCount = parts ? partCount : 0;
    if (parts) { in.partPrim = &parts[0].prim; in.partCount32 = &parts[0].count; in.partStride = sizeof(PrimitiveRunPart); }
    in.vertices = nNew; in.vsize = vsize; in.stream = p; in.fmtRaw = F.raw; in.nAttrs = nA; in.attrs = A; in.fast = fast;
    in.shapeKey = shapeKey_; in.shapeDraw = shapeKey_ ? shapeDraw_++ : 0;   // (Claude v95)
    if (inputObserver_) inputObserver_(in);
  }
  const bool skip = offer && sink_->retainOffer(in, D);
  base = D.v.count;
  if (base > 0) { D.vertexCount += nNew; D.commandCount += H.commandCount; D.prim = GXP_TRIANGLES; }
  if (base == 0) {
    V.hasPos = vcd.pos != GXA_NONE;
    V.hasNrm = vcd.nrm != GXA_NONE;
    V.hasPmIdx = vcd.pmIdx != 0;
    for (int c = 0; c < 2; c++) V.hasClr[c] = vcd.clr[c] != GXA_NONE;
    for (int t = 0; t < 8; t++) { V.hasTex[t] = vcd.tex[t] != GXA_NONE; V.hasTexMtxIdx[t] = vcd.texMtxIdx[t] != 0; }
  }
  if (skip) {   // retained: the sink holds this draw's decoded vertices already (gxretain.h); only the counts advance
    V.count = base + nNew;
    p += size_t(vsize) * nNew;
    draws_ += H.commandCount;
    const uint64_t prof1 = t2pProfTicks();
    sink_->onAppended(regs_, D, base);
    if (t2pProfEnabled()) { g_t2pProf[0] += prof1 - prof0; g_t2pProf[1] += t2pProfTicks() - prof1; g_t2pProf[2] += nNew; g_t2pProf[6] += nNew; }
    return true;
  }
  const uint32_t total = base + nNew;
  if (deferQ_ && base > 0 && deferQ_->pending()) {   // queued jobs may point into this draw's vertex vectors: no reallocation under them
    bool grow = (V.hasPos && V.pos.capacity() < size_t(total) * 3) || (V.hasNrm && V.nrm.capacity() < size_t(total) * 3) || (V.hasPmIdx && V.pmIdx.capacity() < total);
    for (int c = 0; c < 2; c++) grow = grow || (V.hasClr[c] && V.clr[c].capacity() < size_t(total) * 4);
    for (int t = 0; t < 8; t++) grow = grow || (V.hasTex[t] && V.tex[t].capacity() < size_t(total) * 2) || (V.hasTexMtxIdx[t] && V.texMtxIdx[t].capacity() < total);
    if (grow) finishDecodes();
  }
  V.count = total;
  if (V.hasPos) resizeDecoded(V.pos,size_t(total)*3);
  if (V.hasNrm) resizeDecoded(V.nrm,size_t(total)*3);
  if (V.hasPmIdx) resizeDecoded(V.pmIdx,total);
  for (int c = 0; c < 2; c++) if (V.hasClr[c]) resizeDecoded(V.clr[c],size_t(total)*4);
  for (int t = 0; t < 8; t++) {
    if (V.hasTex[t]) resizeDecoded(V.tex[t],size_t(total)*2);
    if (V.hasTexMtxIdx[t]) resizeDecoded(V.texMtxIdx[t],total);
  }
  for (int k = 0; k < nA; ++k) {
    AttrPlan& a = A[k];
    switch (a.kind) {
      case AttrPlan::Byte: a.out = a.slot ? V.texMtxIdx[a.slot-1].data()+base : V.pmIdx.data()+base; break;
      case AttrPlan::Pos: a.out = reinterpret_cast<uint8_t*>(V.pos.data()+size_t(base)*3); break;
      case AttrPlan::Nrm: a.out = reinterpret_cast<uint8_t*>(V.nrm.data()+size_t(base)*3); break;
      case AttrPlan::Clr: a.out = V.clr[a.slot].data()+size_t(base)*4; break;
      case AttrPlan::Tex: a.out = reinterpret_cast<uint8_t*>(V.tex[a.slot].data()+size_t(base)*2); break;
    }
  }
  if (splitRun_ && !fast) {   // (Claude v79) the generic decode walks one contiguous stream: copy the parts as before
    runBytes_.resize(size_t(nNew) * vsize); size_t at = 0;
    for (int i = 0; i < partCount; i++) { const size_t bytes = size_t(parts[i].count) * vsize; std::memcpy(runBytes_.data() + at, parts[i].data, bytes); at += bytes; }
    p = runBytes_.data(); splitRun_ = false;
  }
  const uint8_t* cacheStream = p;
  const uint32_t cacheBytes = vsize * nNew;
  const bool cached = fast && decodeCache_ && decodeCache_->lookup(F.raw,p,cacheBytes,A,nA,nNew,vsize,draws_);
  if (cached) p += cacheBytes;
  // (Claude) the fast attribute decode; benchmark knob g_benchRepeat[8] runs it extra times (same output) to cost it
  auto decodeFast = [&]() { if (!decodeAttrsFast(A, nA, p, vsize, nNew, fastAttributeDecode_)) arrayError = true; };
  if (fast && !cached && splitRun_) {   // (Claude v79) part by part, each from its own place in the stream, into consecutive output vertices
    for (int i = 0; i < partCount; i++) {
      if (!decodeAttrsFast(A, nA, parts[i].data, vsize, parts[i].count, fastAttributeDecode_)) arrayError = true;
      for (int k = 0; k < nA; k++) A[k].out += size_t(parts[i].count) * A[k].outStride;
    }
  } else if (fast && !cached) {
    for (int rr = 0; rr < g_benchRepeat[8]; rr++) decodeFast();
    DecodeQueue* q = deferPaused_ ? nullptr : deferQ_.get();   // (Claude v81) paused: decoded here, the helper finds no jobs
    int nHeavy = 0;
    g_deferWhy[0]++; if (!q) g_deferWhy[1]++; if (!target) g_deferWhy[2]++; if (offer) g_deferWhy[3]++; if (inputObserver_) g_deferWhy[4]++; if (decodeCache_) g_deferWhy[5]++; 
    for (int k = 0; k < nA; k++) if (A[k].kind != AttrPlan::Byte) nHeavy++;
    const size_t streamBytes = size_t(vsize) * nNew;
    if (nHeavy <= 0 || nHeavy > 12) g_deferWhy[2]++; if (uint32_t(nHeavy) * nNew < g_deferMin) g_deferWhy[3]++; g_deferWhy[6] += uint32_t(nHeavy) * nNew; if (streamBytes > DecodeQueue::kArena) g_deferWhy[4]++;
    if (q && target && !offer && !inputObserver_ && !decodeCache_ && g_benchRepeat[8] == 0 && nHeavy > 0 && nHeavy <= 12 && uint32_t(nHeavy) * nNew >= g_deferMin && streamBytes <= DecodeQueue::kArena) {
      if (q->published.load(std::memory_order_relaxed) - q->done.load() >= DecodeQueue::kJobs || q->arenaUsed + streamBytes > DecodeQueue::kArena) finishDecodes();
      else if (!q->pending()) q->arenaUsed = 0;
      uint8_t* copy = q->arena.data() + q->arenaUsed;
      std::memcpy(copy, p, streamBytes);
      q->arenaUsed += streamBytes;
      const uint32_t id = q->published.load(std::memory_order_relaxed);
      DecodeQueue::Job& j = q->jobs[id % DecodeQueue::kJobs];
      j.nA = 0; j.v0 = copy; j.vsize = vsize; j.n = nNew; j.fastAttr = fastAttributeDecode_;
      for (int k = 0; k < nA; k++) {
        if (A[k].kind != AttrPlan::Byte) j.attrs[j.nA++] = A[k];
        else for (uint32_t v = 0; v < nNew; v++) A[k].out[v] = p[size_t(v) * vsize + A[k].offset];   // the matrix indices stay inline (onAppended reads them)
      }
      q->published.store(id + 1);
      deferredJobs_++;
      g_deferWhy[5]++; g_deferWhy[7] += uint32_t(nHeavy) * nNew;
      if (q->wake) q->wake();
    } else decodeFast();
    p += size_t(vsize) * nNew;
  }
  for (uint32_t i = 0; !fast && i < nNew; i++) {
    for (int k = 0; k < nA; k++) {
      AttrPlan& a = A[k];
      if (a.kind == AttrPlan::Byte) { *a.out = *p; a.out += 1; p += 1; continue; }
      // the element: the stream itself (direct) or an array element (indexed)
      const uint8_t* src = p;
      bool be = true;
      if (a.type != GXA_DIRECT) {
        const uint32_t idx = a.type == GXA_INDEX16 ? rd16(p) : *p;
        be = a.be;
        if (a.base) {
          const uint64_t off = uint64_t(idx) * a.stride;
          if (off + a.need > a.size) {
            arrayError = true;
            if (dbg) std::fprintf(stderr, "array %d index %u stride %u need %u size %u (draw %llu, prim %02x, vat %d)\n", a.arrayId, idx, a.stride, a.need, a.size, (unsigned long long)draws_, cmd, cmd & 7);
            src = nullptr;
          } else {
            src = a.base + off;
          }
        } else {   // not registered by the host: the guest array the CP registers point at (big endian, as on the console)
          src = guestElement(a.arrayId, idx, a.need);
          if (!src) arrayError = true;
        }
      }
      p += a.streamBytes;
      if (a.kind == AttrPlan::Clr) {
        uint8_t* o = a.out;
        if (src) readColor(src, a.fmt, be, o);
        else { o[0] = o[1] = o[2] = 0; o[3] = 255; }
        a.out += 4;
        continue;
      }
      float* o = reinterpret_cast<float*>(a.out);
      a.out += a.outStride;
      if (!src) { o[0] = 0; o[1] = 0; if (a.kind != AttrPlan::Tex) o[2] = 0; continue; }
      const int cb = compBytes(a.fmt);
      for (int c = 0; c < a.nc; c++) o[c] = readOne(src + c * cb, a.fmt, be) * a.scale;
      if (a.kind == AttrPlan::Pos && a.nc == 2) o[2] = 0;
      if (a.kind == AttrPlan::Tex && a.nc == 1) o[1] = 0;
    }
  }
  if (fast && !cached && !arrayError && decodeCache_) decodeCache_->store(F.raw,cacheStream,cacheBytes,A,nA,nNew,vsize);
  if (arrayError) err("draw reads an array element outside the arrays the host registered");

  // primitive assembly (GX winding: quads 0 1 2 / 2 3 0, strips alternate, fans pivot on vertex 0); indices offset by the vertices already in D
  const PrimitiveRunPart single{nullptr,nNew,prim};
  if(!parts){parts=&single;partCount=1;}
  size_t nIdx=0;
  for(int part=0;part<partCount;part++){
    const uint32_t n=parts[part].count;
    switch(parts[part].prim){
      case GXP_QUADS:case GXP_QUADS2:nIdx+=size_t(n/4)*6;break;
      case GXP_TRIANGLES:nIdx+=size_t(n/3)*3;break;
      case GXP_TRISTRIP:case GXP_TRIFAN:nIdx+=n>2?size_t(n-2)*3:0;break;
      default:break;
    }
  }
  const size_t t0=D.tris.size();resizeDecoded(D.tris,t0+nIdx);uint32_t* ti=D.tris.data()+t0;uint32_t b=base;
  for(int part=0;part<partCount;part++){
    const uint32_t n=parts[part].count;
    switch(parts[part].prim){
      case GXP_QUADS:case GXP_QUADS2:
        for(uint32_t v=0;v+3<n;v+=4){const uint32_t q=b+v;ti[0]=q;ti[1]=q+1;ti[2]=q+2;ti[3]=q+2;ti[4]=q+3;ti[5]=q;ti+=6;}break;
      case GXP_TRIANGLES:
        for(uint32_t v=0;v+2<n;v+=3){ti[0]=b+v;ti[1]=b+v+1;ti[2]=b+v+2;ti+=3;}break;
      case GXP_TRISTRIP:
        for(uint32_t v=2;v<n;v++){if((v&1)==0){ti[0]=b+v-2;ti[1]=b+v-1;}else{ti[0]=b+v-1;ti[1]=b+v-2;}ti[2]=b+v;ti+=3;}break;
      case GXP_TRIFAN:
        for(uint32_t v=2;v<n;v++){ti[0]=b;ti[1]=b+v-1;ti[2]=b+v;ti+=3;}break;
      default:break;
    }
    b+=n;
  }
  draws_ += H.commandCount;
  if (offer) sink_->retainDecoded(in, D, base, t0, arrayError);
  const uint64_t prof1 = t2pProfTicks();
  if (target) sink_->onAppended(regs_, D, base);
  else if (sink_) sink_->onDraw(regs_, D);
  if (t2pProfEnabled()) g_t2pProf[0] += prof1 - prof0;               // vertex decode + assembly
  if (t2pProfEnabled()) g_t2pProf[1] += t2pProfTicks() - prof1;      // the sink (batching, state decode)
  if (t2pProfEnabled()) g_t2pProf[2] += nNew;
  return true;
}

namespace {
// GXGetTexBufferSize's tile shapes: log2 of the tile width / height in texels
void tileShift(uint32_t fmt, int& sx, int& sy) {
  switch (fmt) {
    case 0x0: case 0x8: case 0xE: case 0x20: case 0x30: sx = 3; sy = 3; break;                         // I4 C4 CMPR R4 Z4
    case 0x1: case 0x2: case 0x9: case 0x11: case 0x22: case 0x27: case 0x28: case 0x29: case 0x2A:   // I8 IA4 C8 Z8 RA4 A8 R8 G8 B8
    case 0x39: case 0x3A: sx = 3; sy = 2; break;                                                       // Z8M Z8L
    default: sx = 2; sy = 2; break;
  }
}
}  // namespace

uint32_t gxTexBufferSize(uint32_t w, uint32_t h, uint32_t fmt, bool mips) {
  int sx, sy;
  tileShift(fmt, sx, sy);
  const uint32_t tileBytes = (fmt == 0x6 || fmt == 0x16) ? 64 : 32;
  uint32_t n = 0;
  for (;;) {
    n += tileBytes * ((w + (1u << sx) - 1) >> sx) * ((h + (1u << sy) - 1) >> sy);
    if (!mips || (w <= 1 && h <= 1)) break;
    w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
  }
  return n;
}

// One GX_AURORA command (p is past the 0x50 byte). Layouts: aurora include/dolphin/gx/GXAurora.h and lib/gx/command_processor.cpp handle_aurora; big endian
// like the rest of the stream, host pointers as u64 (the 3DS's are 32 bit: the low word).
// (Claude v80) "\x01WWROUTE1 B sl st sw sh dl dt dw dh" or "\x01WWROUTE1 E" (n3ds gpu_3ds.cpp n3dsUiMapBegin / End). Any other marker text is ignored, as before.
// A route marker ends the current batch (the state generation moves on, no register changes): draws on either side never merge.
void GxFifo::uiRouteMarker(const char* text, uint32_t n) {
  static const char kTag[] = "\x01WWROUTE1 ";
  constexpr uint32_t kTagLen = sizeof kTag - 1;
  if (n < kTagLen + 1 || std::memcmp(text, kTag, kTagLen) != 0) return;
  const char* q = text + kTagLen;
  const char* const e = text + n;
  GxUiRoute R;
  if (*q == 'E') R.begin = false;
  else if (*q == 'B') {
    R.begin = true; ++q;
    int v[8];
    for (int i = 0; i < 8; i++) {
      while (q < e && *q == ' ') ++q;
      bool neg = false; if (q < e && *q == '-') { neg = true; ++q; }
      if (q >= e || *q < '0' || *q > '9') return;   // malformed: ignored
      long x = 0; while (q < e && *q >= '0' && *q <= '9' && x < 100000) x = x * 10 + (*q++ - '0');
      v[i] = int(neg ? -x : x);
    }
    for (int i = 0; i < 8; i++) if (v[i] < -32768 || v[i] > 32767) return;
    for (int i = 0; i < 4; i++) { R.src[i] = int16_t(v[i]); R.dst[i] = int16_t(v[4 + i]); }
  } else return;
  stateGen_++;
  if (sink_) sink_->onUiRoute(regs_, R);
}

// Cached instruction layout ONLY: no cached vertices, array bindings or live pointers.
// Every opcode/count and padding byte is revalidated before any draw; indices and
// attributes come from THIS FIFO packet. Reused addresses and edited data are safe.
bool GxFifo::processShapeProgram(uint64_t hint,const uint8_t* data,size_t size) {
  if(!data || !size || size>262144 || !sink_)return false;
  auto& entry=shapePrograms_[((hint>>4)^(hint>>17))&63];
  bool hit=entry && entry->hint==hint && entry->size==size;
  if(hit){const auto& f=vertexFormat(entry->vat);hit=std::memcmp(f.raw,entry->format,sizeof entry->format)==0;}
  if(hit){for(const auto& part:entry->parts)if(data[part.offset]!=part.cmd || rd16(data+part.offset+1)!=part.count){hit=false;break;}}
  if(hit){for(const auto& z:entry->zeros)for(uint32_t k=0;k<z.size;++k)if(data[z.offset+k]){hit=false;break;}}
  if(!hit){
    // Compile off to the side: failure cannot publish partial layout or draw twice.
    std::unique_ptr<ShapeProgram> fresh;
    try {
      fresh=std::make_unique<ShapeProgram>();fresh->hint=hint;fresh->size=size;
      size_t at=0;bool first=true;
      while(at<size){
        if(data[at]==0){const size_t start=at;while(at<size && data[at]==0)++at;fresh->zeros.push_back({uint32_t(start),uint32_t(at-start)});continue;}
        const uint8_t cmd=data[at],prim=cmd&0xf8;
        if(size-at<3 || !(prim==GXP_QUADS||prim==GXP_QUADS2||prim==GXP_TRIANGLES||prim==GXP_TRISTRIP||prim==GXP_TRIFAN))return false;
        if(first){fresh->vat=cmd&7;const auto& f=vertexFormat(fresh->vat);std::memcpy(fresh->format,f.raw,sizeof fresh->format);first=false;}
        if((cmd&7)!=fresh->vat)return false;
        const uint32_t count=rd16(data+at+1),vs=vertexFormat(fresh->vat).vsize;
        if(!vs || count<(prim==GXP_QUADS||prim==GXP_QUADS2?4u:3u))return false;
        const uint64_t next=at+3+uint64_t(count)*vs;if(next>size)return false;
        fresh->parts.push_back({uint32_t(at),count,uint32_t(next),cmd});at=size_t(next);
      }
      if(first)return false;
    }catch(...){return false;}
    const size_t old=entry?entry->cost():0;
    if(fresh->cost()>512*1024-(shapeProgramBytes_-old))return false;
    shapeProgramBytes_=shapeProgramBytes_-old+fresh->cost();entry=std::move(fresh);++shapeProgramMisses_;
  }else ++shapeProgramHits_;
  // Same primitive-run grouping/512-vertex cap as tryDrawRun, including padding
  // boundaries and the sink's remaining batch capacity. Only packet decoding differs.
  size_t i=0;const uint32_t vs=vertexFormat(entry->vat).vsize;
  while(i<entry->parts.size()){
    const auto& first=entry->parts[i];size_t j=i;uint32_t total=0;int parts=0;
    const uint32_t limit=primitiveRuns_?std::min<uint32_t>(512,sink_->primitiveRunLimit()):0;
    if(limit>=6)while(j<entry->parts.size() && parts<int(runParts_.size())){
      const auto& op=entry->parts[j];
      if((j>i && op.offset!=entry->parts[j-1].end) || op.count>limit-total)break;
      runParts_[parts++]={data+op.offset+3,op.count,uint8_t(op.cmd&0xf8)};total+=op.count;++j;
    }
    if(parts<2){const uint8_t* p=data+first.offset+3;if(!handleDraw(first.cmd,p,data+first.end,int(first.count)))return true;++i;}
    else {
      runBytes_.resize(size_t(total)*vs);size_t at=0;
      for(int k=0;k<parts;++k){const size_t bytes=size_t(runParts_[k].count)*vs;std::memcpy(runBytes_.data()+at,runParts_[k].data,bytes);at+=bytes;}
      const uint8_t* p=runBytes_.data();if(!handleDraw(first.cmd,p,p+runBytes_.size(),int(total),runParts_.data(),parts))return true;i=j;
    }
  }
  return true;
}

// Direct execution of PC-prepared geometry runs. Same batching boundaries as
// tryDrawRun; current vertex arrays, state and poses are still read at use time.
// No GX primitive header walk, run-vector construction or stream concatenation.
bool GxFifo::executePreparedShape(const PreparedShape& program){
 struct Depth {int& value;Depth(int& x):value(x){++value;}~Depth(){--value;}}depth(processDepth_);
 if(processDepth_==1)++processSerial_;
 for(size_t i=0;i<program.ops.size();){
  const auto& op=program.ops[i];const uint32_t limit=primitiveRuns_&&sink_?std::min<uint32_t>(512,sink_->primitiveRunLimit()):0;
  const size_t end=limit==512?program.ends512[i]:program.endFor(i,limit);
  const uint8_t* data=program.stream.data()+op.flat;
  if(end<i+2){if(!handleDraw(op.cmd,data,data+size_t(op.count)*program.vsize,int(op.count)))return true;++i;}
  else {const uint32_t count=program.prefix[end]-program.prefix[i];
   if(!handleDraw(op.cmd,data,data+size_t(count)*program.vsize,int(count),program.parts.data()+i,int(end-i)))return true;i=end;}
  ++preparedShapeRuns_;
 }
 return true;
}

bool GxFifo::processShapeRef(const uint8_t* dl,uint32_t size,uint32_t gen) {
  // Key: the list (address, size, generation, first/last 16 bytes) + the vertex format of its first primitive + base/stride of every indexed array
  // that format reads. All primitives of a J3D shape list use one format (the producer's shape lists are pure primitive streams).
  ++shapeRefs_;
  if(!shapeRefsOn_ || size<3 || shapeKey_){process(dl,size);return true;}
  uint64_t h=1469598103934665603ull;
  auto mix=[&h](const void* q,size_t n){const uint8_t* b=static_cast<const uint8_t*>(q);for(size_t i=0;i<n;i++){h^=b[i];h*=1099511628211ull;}};
  const uintptr_t at=reinterpret_cast<uintptr_t>(dl);mix(&at,sizeof at);mix(&size,4);mix(&gen,4);
  mix(dl,std::min<uint32_t>(16,size));mix(dl+size-std::min<uint32_t>(16,size),std::min<uint32_t>(16,size));
  const auto& f=vertexFormat(dl[0]&7);mix(f.raw,sizeof f.raw);
  const uint32_t vcdLo=f.raw[0],vcdHi=f.raw[1];
  for(int id=0;id<12;id++){
    const uint32_t t=id<4?(vcdLo>>(9+2*id))&3:(vcdHi>>(2*(id-4)))&3;
    if(t<2)continue;   // not indexed
    const GxArray& a=arrays_[id];const uintptr_t b=reinterpret_cast<uintptr_t>(a.data);
    mix(&id,sizeof id);mix(&b,sizeof b);mix(&a.bigEndian,1);
    const uint32_t stride=a.strideFromCp?regs_.cp[0xB0+id]:a.stride;mix(&stride,4);
  }
  if(!h)h=1;
  shapeKey_=h;shapeDraw_=0;
  if(preparedShapes_&&!deferredDecodeActive()){
   std::array<uint32_t,5> format;std::memcpy(format.data(),f.raw,sizeof f.raw);
   if(const auto* program=preparedShapes_->get(dl,size,gen,format)){
    if(program->vsize==f.vsize){executePreparedShape(*program);shapeKey_=0;return true;}
   }
  }
  process(dl,size);
  shapeKey_=0;
  return true;
}

void prepareStateProgram(GxStateProgram& p) {
  for(auto& op:p.ops)if(op.kind==0x61)op.offset=stateDirtyBp(op.value>>24);
}

bool GxFifo::processStateProgram(const uint8_t* data,size_t size) {
  // Content, not a live J3D pointer, identifies a program. Mutation/collision is a miss.
  if(!data || !size || size>2048)return false;
  uint32_t hash=2166136261u;size_t i=0;
  for(;i+4<=size;i+=4){uint32_t v;std::memcpy(&v,data+i,4);hash=(hash^v)*0x9e3779b1u;}
  for(;i<size;++i)hash=(hash^data[i])*16777619u;
  hash^=hash>>16;
  const size_t first=(hash&63)*2;
  size_t slot=first;
  for(size_t k=first;k<first+2;++k){auto& e=statePrograms_[k];
    if(e && e->bytes.size()==size && std::memcmp(e->bytes.data(),data,size)==0){
      stateProgramAges_[k]=++stateProgramClock_;++stateProgramHits_;e->apply(*this);return true;
    }
    if(!e || (statePrograms_[slot] && stateProgramAges_[k]<stateProgramAges_[slot]))slot=k;
  }
  auto& entry=statePrograms_[slot];
  {
    if(!GxStateProgram::valid(data,size))return false;
    if(entry)stateProgramBytes_-=entry->capacityBytes();
    try {
      if(!entry)entry=std::make_unique<GxStateProgram>();
      entry->compile(data,size);
      for(auto& op:entry->ops)if(op.kind==0x61)op.offset=stateDirtyBp(op.value>>24);
    } catch (...) { entry.reset();return false; } // no partial cache entry or register effects
    const size_t cost=entry->capacityBytes();
    if(cost>256*1024-stateProgramBytes_){entry.reset();return false;}
    stateProgramBytes_+=cost;stateProgramAges_[slot]=++stateProgramClock_;++stateProgramMisses_;
  }
  entry->apply(*this);return true;
}

bool GxFifo::handleAurora(const uint8_t*& p, const uint8_t* end) {
  auto need = [&](size_t n) { if (size_t(end - p) < n) { err("truncated GX_AURORA command"); p = end; return false; } return true; };
  if (!need(2)) return false;
  const uint32_t sub = rd16(p);
  p += 2;
  auto bpSet = [&](uint32_t id, uint32_t v) {
    setReg(regs_.bp[id], (id << 24) | (v & 0xFFFFFF), stateDirtyBp(id));
    if (!regs_.bpWritten[id]) { regs_.bpWritten[id] = true; touchStatic();stateGen_++; stateDirty_ |= stateDirtyBp(id); }
  };
  if (sub >= 0x10 && sub <= 0x1F) {                       // LOAD_ARRAYBASE + id: u64 address, u32 size, u8 little endian
    if (!need(13)) return false;
    const int id = int(sub & 0xF);
    const uint32_t addr = rd32(p + 4), size = rd32(p + 8);
    const bool le = p[12] == 1;
    p += 13;
    regs_.cp[0xA0 + id] = addr;
    GxArray a;
    a.size = uncheckedArrays_ && size ? 0x7FFFFFFFu : size;   // live game: the declared sizes are placeholders (3 MiB, or one element short)
    a.bigEndian = !le;
    a.strideFromCp = true;
    a.data = (addr && size && guestRead_) ? guestRead_(addr, size) : nullptr;
    if (addr && size && !a.data) err("GX_AURORA array outside guest memory");
    { static const bool dbg = std::getenv("GXFIFO_ARRAYDBG") != nullptr; if (dbg) std::fprintf(stderr, "ARRAYBASE %d addr %08x size %u le %d (draw %llu)\n", id, addr, size, int(le), (unsigned long long)draws_); }
    arrays_[id] = a;
    return true;
  }
  switch (sub) {
#if defined(__3DS__) || defined(T2P_TEST_MATERIAL_REFS)
    case 0x7A: { // owned immutable material program; queue holds its lease
      if(!need(8))return false;
      const uint64_t token=(uint64_t(rd32(p))<<32)|rd32(p+4);p+=8;
      const auto* program=n3dsMaterialBorrow(token);
      if(!program){err("invalid material program lease");return false;}
      const bool applied=program->apply(*this);++materialRefs_;n3dsMaterialRelease(token);if(!applied){err("invalid prepared material arguments");return false;}return true;
    }
#endif
    case 0x7B: { // owned native-bit pose rows, before the normal dirty/shape path
      if(!need(2))return false;const unsigned count=rd16(p);p+=2;
      if(!count||count>24){err("invalid owned pose count");return false;}
      const uint8_t* q=p;
      // Validate the whole record before touching canonical registers.
      for(unsigned i=0;i<count;++i){if(size_t(end-q)<4){err("truncated owned pose header");return false;}const unsigned a=rd16(q),n=rd16(q+2);q+=4;
        if((n!=9&&n!=12)||a+n>0x600||size_t(end-q)<n*4){err("invalid owned pose rows");return false;}q+=n*4;}
      for(unsigned i=0;i<count;++i){const unsigned a=rd16(p),n=rd16(p+2);p+=4;
        if(g_gxOriginProf){g_gxOriginCount[g_gxOrigin][2]++;g_gxOriginCount[g_gxOrigin][3]+=n;}
        for(unsigned j=0;j<n;++j)setReg(regs_.xf[a+j],le32(p+j*4),kStateDirtyXfMtx);p+=n*4;}
      ++modelGroups_;return true;
    }
    case 0x7C: { // ordered model group: shape ref + indexed PNGP matrix slots
      if(!need(17))return false;
      const uint32_t addr=rd32(p+4),n=rd32(p+8),gen=rd32(p+12),count=p[16];
      if(!count||count>10){err("invalid model matrix count");return false;}
      if(!need(17+count*2))return false;
      const uint8_t* dl=(addr&&n&&n<=262144&&guestRead_)?guestRead_(addr,n):nullptr;
      if(!dl){err("model group outside guest memory");p+=17+count*2;return true;}
      // Execute the same indexed loads, in the same slot order and with the
      // same per-register generations/dirty bits as the raw GX fallback.
      for(uint32_t i=0;i<count;++i){const uint16_t index=rd16(p+17+i*2);if(index==0xffff)continue;
        if(g_gxOriginProf)g_gxOriginCount[g_gxOrigin][4]+=2;
        indexedXf(GXAR_POSMTX,index,uint16_t(0xb000+i*12));
        indexedXf(GXAR_NRMMTX,index,uint16_t(0x8400+i*9));
      }
      p+=17+count*2;++modelGroups_;processShapeRef(dl,n,gen);return true;
    }
    case 0x7D: { // (Claude v95) J3D shape list by reference: u64 address, u32 size, u32 model generation
      if(!need(16))return false;
      const uint32_t addr=rd32(p+4),n=rd32(p+8),gen=rd32(p+12);p+=16;
      const uint8_t* dl=(addr&&n&&guestRead_)?guestRead_(addr,n):nullptr;
      if(!dl){err("GX_AURORA shape reference outside guest memory");return true;}
      processShapeRef(dl,n,gen);return true;
    }
    case 0x7F: { // Windfall shape layout: u64 identity HINT + u32 size + owned DL bytes.
      if(!need(12))return false;const uint64_t hint=(uint64_t(rd32(p))<<32)|rd32(p+4);const uint32_t n=rd32(p+8);p+=12;
      if(!need(n))return false;
      if(!processShapeProgram(hint,p,n))process(p,n);
      p+=n;return true;
    }
    case 0x7E: { // Windfall J3D material: u32 byte count + OWNED original DL bytes.
      if(!need(4))return false;const uint32_t n=rd32(p);p+=4;
      if(!need(n))return false;
      // Invalid/unsupported programs remain normal GX; never partially execute twice.
      if(!processStateProgram(p,n))process(p,n);
      p+=n;return true;
    }

    case 0x01: return need(24) && (p += 24, true);          // LOAD_VIEWPORT_RENDER (PC render-target coordinates: the XF viewport is used)
    case 0x02: return need(16) && (p += 16, true);          // LOAD_SCISSOR_RENDER
    case 0x03: err("GX_AURORA_LOAD_PROJECTION_FULL is not supported"); return need(64) && (p += 64, true);
    case 0x20: case 0x22: {                                 // debug group push / marker: u16 length + text
      if (!need(2)) return false;
      const uint32_t n = rd16(p);
      if (!need(2 + n)) return false;
      if (sub == 0x22) uiRouteMarker(reinterpret_cast<const char*>(p + 2), n);
      p += 2 + n;
      return true;
    }
    case 0x21: case 0x38: case 0x3A: return true;          // debug group pop, depth snapshot, end offscreen
    case 0x30: {                                            // LOAD_TEXOBJ: u8 map, u64 data, u32 w, h, fmt, tlut, u8 mips, u32 object id, data version
      if (!need(34)) return false;
      const int m = p[0] & 7;
      const uint32_t addr = rd32(p + 5), fmt = rd32(p + 17), tlut = rd32(p + 21);
      p += 34;
      setReg(regs_.texAddr[m], addr, kStateDirtyTex);
      if (fmt == 0x8 || fmt == 0x9 || fmt == 0xA) {         // CI: TX_SETTLUT's format comes from the palette slot on the console (GXLoadTexObj writes the region's register)
        const int lo = m < 4 ? m : m - 4, id = 0x98 + (m < 4 ? 0 : 0x20) + lo;
        bpSet(uint32_t(id), (tlut & 0x3FF) | (uint32_t(auroraTlutFmt_[tlut & 31] & 3) << 10));
      }
      return true;
    }
    case 0x31: {                                            // LOAD_TLUT: u8 slot, u64 data, u32 fmt, u16 entries, u32 object id, data version
      if (!need(23)) return false;
      const uint32_t slot = p[0], addr = rd32(p + 5), fmt = rd32(p + 9), entries = rd16(p + 13);
      p += 23;
      auroraTlutFmt_[slot & 31] = uint8_t(fmt & 3);
      for (int m = 0; m < 8; m++) {                         // maps bound to this slot see its format
        const int lo = m < 4 ? m : m - 4, base = m < 4 ? 0 : 0x20, idT = 0x98 + base + lo, idI = 0x88 + base + lo;
        const uint32_t f = (regs_.bp[idI] >> 20) & 15;
        if ((f == 8 || f == 9 || f == 10) && (regs_.bp[idT] & 0x3FF) == slot) bpSet(uint32_t(idT), (regs_.bp[idT] & 0x3FF) | ((fmt & 3) << 10));
      }
      regs_.tlutSrcAddr = addr;
      bpSet(0x64, (addr >> 5) & 0x1FFFFF);
      bpSet(0x65, (slot & 0x3FF) | (((entries + 15) / 16) << 10));
      if (sink_ && addr) sink_->onTlutLoad(regs_);
      return true;
    }
    case 0x32: case 0x33: return need(4) && (p += 4, true);   // destroy texture / palette object (the cache verifies by content)
    case 0x34: return need(8) && (p += 8, true);              // destroy copy texture
    case 0x35: {                                            // LOAD_COPY_SRC: s32 left, top, width, height
      if (!need(16)) return false;
      const uint32_t l = rd32(p), t = rd32(p + 4), w = rd32(p + 8), h = rd32(p + 12);
      p += 16;
      bpSet(0x49, (l & 0x3FF) | ((t & 0x3FF) << 10));
      bpSet(0x4A, ((w - 1) & 0x3FF) | (((h - 1) & 0x3FF) << 10));
      return true;
    }
    case 0x36: {                                            // LOAD_COPY_DST: u32 width, height, format, u8 mipmap (GXSetTexCopyDst)
      if (!need(13)) return false;
      const uint32_t w = rd32(p), h = rd32(p + 4), fmt = rd32(p + 8);
      const bool mip = p[12] != 0;
      p += 13;
      uint32_t pe = fmt & 0xF;
      if (fmt == 0x13) pe = 0xB;                            // Z16
      const bool inten = fmt <= 3 || fmt == 0x26;           // I4 I8 IA4 IA8 YUVA8
      auroraCopyBits_ = (((pe >> 3) & 1) << 3) | ((pe & 7) << 4) | (mip ? 1u << 9 : 0u) | ((inten ? 3u : 2u) << 15);
      auroraCopyZ_ = (fmt & 0x10) != 0;
      int sx, sy;
      tileShift(fmt, sx, sy);
      const uint32_t rowTiles = (w + (1u << sx) - 1) >> sx, cmp = (fmt == 0x6 || fmt == 0x16) ? 2 : 1;
      bpSet(0x4D, (rowTiles * cmp) & 0x3FF);
      return true;
    }
    case 0x37: {                                            // LOAD_COPY_DEST: u64 address
      if (!need(8)) return false;
      const uint32_t addr = rd32(p + 4);
      p += 8;
      regs_.copyDstAddr = addr;
      bpSet(0x4B, (addr >> 5) & 0x1FFFFF);
      return true;
    }
    case 0x39: err("GX_AURORA offscreen framebuffers are not supported"); return need(8) && (p += 8, true);
    case 0x40: {                                            // DRAW_SIZED: u8 draw opcode, u32 byte length, vertex data
      if (!need(5)) return false;
      const uint8_t cmd = p[0];
      const uint32_t len = rd32(p + 1);
      p += 5;
      if (len == 0) return true;
      const uint32_t vs = vertexFormat(cmd & 7).vsize;
      if (vs == 0 || len % vs) { err("GX_AURORA_DRAW_SIZED length is not a whole number of vertices"); p = end; return false; }
      if (size_t(end - p) < len) { err("truncated GX_AURORA_DRAW_SIZED"); p = end; return false; }
      const uint8_t* q = p;
      const bool ok = handleDraw(cmd, q, p + len, int(len / vs));
      p += len;
      return ok;
    }
    case 0x41: err("GX_AURORA_DRAW_INDEXED is not supported"); p = end; return false;
    case 0x50: return need(4) && (p += 4, true);             // content scale (PC dynamic resolution)
    case 0x51: {                                            // view delta (PC 60 fps interpolation)
      if (!need(1)) return false;
      const bool on = p[0] != 0;
      if (!need(on ? 97 : 1)) return false;
      p += on ? 97 : 1;
      return true;
    }
    case 0x1000: return need(20) && (p += 20, true);        // polygon offset
    default:
      err("unknown GX_AURORA subcommand " + std::to_string(sub));
      p = end;
      return false;
  }
}

size_t GxFifo::process(const uint8_t* data, size_t size) {
  struct Depth { int& d; explicit Depth(int& x) : d(x) { d++; } ~Depth() { d--; } } depth(processDepth_);
  if (processDepth_ == 1) processSerial_++;   // display lists are processed inside their caller's call
  const uint8_t* p = data;
  const uint8_t* end = data + size;
  while (p < end) {
    const uint8_t cmd = *p;
    const uint8_t op = cmd & 0xF8;
    auto need = [&](size_t n) { return size_t(end - p) >= n; };
    if (cmd == 0x00) { p++; continue; }
    if(primitiveRuns_ && (cmd>=0x80 || (aurora_&&cmd==0x50&&end-p>=3&&rd16(p+1)==0x40))){
      const int run=tryDrawRun(p,end);if(run>0)continue;if(run<0)break;
    }
    if (cmd >= 0x80) {
      p++;
      if (!handleDraw(cmd, p, end)) break;
      continue;
    }
    switch (op) {
      case 0x08:   // CP register
        if (!need(6)) { err("truncated CP load"); return size_t(p - data); }
        if (g_gxOriginProf) g_gxOriginCount[g_gxOrigin][1]++;
        handleCp(p[1], rd32(p + 2));
        p += 6;
        break;
      case 0x10: { // XF load
        if (!need(5)) { err("truncated XF load header"); return size_t(p - data); }
        const uint32_t hdr = rd32(p + 1);
        const uint32_t count = (hdr >> 16) + 1, addr = hdr & 0xFFFF;
        if (!need(5 + size_t(count) * 4)) { err("truncated XF load data"); return size_t(p - data); }
        if (g_gxOriginProf) { g_gxOriginCount[g_gxOrigin][2]++; g_gxOriginCount[g_gxOrigin][3] += count; }
        handleXf(addr, p + 5, count);
        p += 5 + size_t(count) * 4;
        break;
      }
      case 0x20: case 0x28: case 0x30: case 0x38:   // indexed XF load A..D
        if (!need(5)) { err("truncated indexed XF load"); return size_t(p - data); }
        if (g_gxOriginProf) g_gxOriginCount[g_gxOrigin][4]++;
        indexedXf(GXAR_POSMTX + (op - 0x20) / 8, uint16_t(rd16(p + 1)), uint16_t(rd16(p + 3)));
        p += 5;
        break;
      case 0x40:   // call display list (address + size)
        if (!need(9)) { err("truncated display list call"); return size_t(p - data); }
        if (sink_) {
          const uint32_t addr = rd32(p + 1), len = rd32(p + 5);
          const uint8_t* dl = sink_->resolveDisplayList(addr, len);
          if (dl) process(dl, len);
          else err("display list call at an address the host cannot resolve");
        }
        p += 9;
        break;
      case 0x50:   // GX_AURORA
        if (!aurora_) { err("GX_AURORA command in a console stream"); return size_t(p - data); }
        p++;
        if (!handleAurora(p, end)) return size_t(p - data);
        break;
      case 0x48:   // invalidate vertex cache
        p++;
        break;
      case 0x60:   // BP register (0x61)
        if (cmd != 0x61) { err("unknown opcode " + std::to_string(cmd)); return size_t(p - data); }
        if (!need(5)) { err("truncated BP load"); return size_t(p - data); }
        {
          const uint32_t v = rd32(p + 1);
          if (g_gxOriginProf) g_gxOriginCount[g_gxOrigin][0]++;
          handleBp(v);
          if ((v >> 24) == 0x45) {   // PE_DONE
            finishDecodes();
            if (sink_) sink_->onDrawDone(regs_);
            if (stopAtDrawDone_) { drawDoneHit_ = true; return size_t(p + 5 - data); }
          }
        }
        p += 5;
        break;
      default:
        err("unknown opcode " + std::to_string(cmd));
        return size_t(p - data);
    }
  }
  return size_t(p - data);
}

}  // namespace t2p
