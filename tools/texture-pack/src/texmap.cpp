#include <cstdio>
#include "texmap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace t2p {
namespace {

int log2i(int n) { int l = 0; while ((1 << (l + 1)) <= n) l++; return l; }
std::string num(int v) { return std::to_string(v); }

}  // namespace

PicaSamplerDesc samplerFor(const GxTexBinding& b, int levels) {
  PicaSamplerDesc S;
  S.wrapS = planAxis(b.width, b.wrapS).wrap;
  S.wrapT = planAxis(b.height, b.wrapT).wrap;
  S.magLinear = b.magLinear;
  S.minLinear = b.minLinear;
  S.lodBias = b.lodBias;
  if (levels > 1) {
    S.mipFilter = b.mipMode == 2 ? 2 : 1;
    S.minLevel = std::min(levels - 1, std::max(0, int(std::floor(b.minLod))));
    S.maxLevel = std::min(levels - 1, std::max(S.minLevel, int(std::ceil(b.maxLod))));
  }
  return S;
}

size_t gxImageBytes(const GxTexBinding& b) {
  size_t total = 0;
  const int levels = b.mipLevels();
  for (int l = 0; l < levels; l++) total += gxTextureBytes(b.fmt, std::max(1, b.width >> l), std::max(1, b.height >> l));
  return total;
}

MappedTexture mapTexture(const GxTexBinding& b, const uint8_t* image, size_t imageSize, const GxTlut* tlut, const TexMapOptions& opt) {
  struct TotalClock { uint64_t t; ~TotalClock() { const uint64_t elapsed=texProfTicks()-t;g_texProf[0]+=elapsed;g_texProf[7]=std::max(g_texProf[7],elapsed);g_texProf[5]++; } } totalClock{texProfTicks()};
  MappedTexture M;
  if (b.width < 1 || b.height < 1 || b.width > 1024 || b.height > 1024) { M.error = "texture size " + num(b.width) + "x" + num(b.height) + " outside 1..1024"; return M; }
  const AxisPlan px = planAxis(b.width, b.wrapS), py = planAxis(b.height, b.wrapT);
  M.width = px.D;
  M.height = py.D;
  // the image occupies n of the D texels (padded / tiled axes): GX coordinate 1 = the whole image = n/D of the PICA coordinate range
  M.scaleS = axisScale(px);
  M.scaleT = axisScale(py);
  if (!px.exact) { M.exact = false; M.notes.push_back("S axis: " + px.note); }
  if (!py.exact) { M.exact = false; M.notes.push_back("T axis: " + py.note); }

  // ---- how many levels can the PICA keep? (chain stops at 8x8; padded axes must keep the same n/D ratio)
  int gxLevels = b.mipLevels();
  const int picaMax = log2i(std::min(M.width, M.height)) - 3 + 1;
  int levels = std::min(gxLevels, std::max(1, picaMax));
  auto ratioOk = [&](const AxisPlan& p, int l) {
    if (p.fill != AxisPlan::EdgePad) return true;
    const int nl = std::max(1, p.n >> l), Dl = p.D >> l;
    return int64_t(nl) * p.D == int64_t(p.n) * Dl;
  };
  while (levels > 1 && !(ratioOk(px, levels - 1) && ratioOk(py, levels - 1))) levels--;
  if (levels < gxLevels) {
    M.exact = false;
    M.notes.push_back("mip chain cut from " + num(gxLevels) + " to " + num(levels) + " levels (the PICA chain stops at 8x8" +
                      (levels < std::min(gxLevels, picaMax) ? ", and padded sizes lose the n/D ratio below that" : "") + ")");
  }
  M.levels = levels;

  // ---- decode and fit every level
  const uint64_t tDecode = texProfTicks();
  std::vector<MipImage> imgs;
  size_t offset = 0;
  for (int l = 0; l < levels; l++) {
    const int nw = std::max(1, b.width >> l), nh = std::max(1, b.height >> l);
    const size_t bytes = gxTextureBytes(b.fmt, nw, nh);
    if (offset + bytes > imageSize) { M.error = "level " + num(l) + " runs past the image data"; return M; }
    std::vector<uint8_t> src;
    std::string err;
    if (!decodeGxTexture(b.fmt, nw, nh, image + offset, bytes, tlut, src, err)) { M.error = err; return M; }
    offset += bytes;
    MipImage m;
    m.w = M.width >> l;
    m.h = M.height >> l;
    // Already power-of-two and fitted: ownership can move, with identical texels.
    if (m.w == nw && m.h == nh) m.rgba = std::move(src);
    else if (size_t(m.w) * m.h * 4 >= (256u << 10)) {
      // Padding/stretching must not require a multi-MiB temporary beside the
      // decoded source and final texture. The encoder visits these exact maps.
      m.rgba = std::move(src); m.sourceWidth = nw;
      m.sourceX.resize(m.w); m.sourceY.resize(m.h);
      for (int x=0;x<m.w;++x) m.sourceX[x]=axisSource(px,x,nw,m.w);
      for (int y=0;y<m.h;++y) m.sourceY[y]=axisSource(py,y,nh,m.h);
    } else {
    m.rgba.resize(size_t(m.w) * m.h * 4);
    for (int y = 0; y < m.h; y++) {
      const int sy = axisSource(py, y, nh, m.h);
      for (int x = 0; x < m.w; x++) {
        const int sx = axisSource(px, x, nw, m.w);
        std::memcpy(&m.rgba[(size_t(y) * m.w + x) * 4], &src[(size_t(sy) * nw + sx) * 4], 4);
      }
    }
    }
    imgs.push_back(std::move(m));
  }

  g_texProf[1] += texProfTicks() - tDecode;
  TexConvOptions co = opt.conv;
  co.needAlpha = opt.needAlpha;
  PicaTexture T = chooseFormatLevels(imgs, co);
#ifdef TEXHASH_DUMP
  { uint64_t h = 1469598103934665603ull; for (uint8_t c : T.data) h = (h ^ c) * 1099511628211ull; std::fprintf(stderr, "texhash fmt %d exact %d n %zu %016llx\n", T.fmt, int(T.exact), T.data.size(), (unsigned long long)h); }
#endif
  M.fmt = T.fmt;
  M.encodedBytes=T.encodedBytes;M.externalOutput=T.externalOutput;
  M.data = std::move(T.data);
  M.levelOffset = T.levelOffset;
  M.maxErr255 = T.maxErr255;
  M.meanErr255 = T.meanErr255;
  if (!T.exact) { M.exact = false; M.notes.push_back(T.note); }
  else if (!T.note.empty()) M.info.push_back(T.note);

  // ---- sampler
  M.sampler = samplerFor(b, levels);
  if (levels > 1) M.info.push_back("mip level selection (LOD formula, bias, trilinear weights) of the PICA is unverified against the GX unit");
  // (a chain cut down to one level was already logged above; maxLod 0 means GX never leaves level 0 either)
  if (b.magLinear || b.minLinear) M.info.push_back("bilinear weights of the PICA vs the GX unit are unverified");
  if (b.aniso) M.info.push_back("anisotropic filtering ignored");
  M.ok = true;
  return M;
}

}  // namespace t2p
