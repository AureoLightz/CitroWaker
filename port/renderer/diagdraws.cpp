// diagdraws (PC): which (merged) draw covers which EFB pixel in one recorded frame, and what each draw's material is.
//   usage: diagdraws replay.wwrp frameNo out_prefix
//   out_prefix.ids  : 640*480 int32, the draw index nearest the camera at each EFB pixel (-1 none). Depth test only; no blending / alpha test.
//   out_prefix.txt  : one line per draw: index, gx draws merged, vertices, covered pixels, numChans, texgens, stages, colour channel, textures, notes
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gxbatch.h"
#include "gxfifo.h"
#include "gxstate.h"
#include "texcache.h"
#include "texconv.h"
#include "picaasm.h"
#include "submit.h"
#include "draw.h"
#include "vtx.h"

using namespace t2p;

struct Mem1 : GuestMem {
  std::vector<uint8_t> bytes;
  const uint8_t* read(uint32_t addr, size_t size) const override {
    addr &= 0x03FFFFFF;
    if (size_t(addr) + size > bytes.size()) return nullptr;
    return &bytes[addr];
  }
};

struct Sink : GxSink {
  Mem1* mem = nullptr;
  const GxFifo* fifo = nullptr;
  struct D { GxState st; GxDraw d; int merged = 1; };
  std::vector<D> draws;
  bool lastIsDraw = false;
  uint64_t lastGen = ~0ull;
  void onDraw(const GxRegs& r, const GxDraw& d) override {
    const uint64_t gen = fifo->stateGeneration();
    if (lastIsDraw && gen == lastGen && canAppend(draws.back().d, d)) {
      D& x = draws.back();
      const int before = maxPosMtx(x.d);
      appendDraw(x.d, d);
      x.merged++;
      if (maxPosMtx(d) > before) x.st = decodeState(r, &x.d);
      return;
    }
    draws.push_back({decodeState(r, &d), d, 1});
    lastIsDraw = true; lastGen = gen;
  }
  void onCopy(const GxRegs& r) override { lastIsDraw = false; const GxCopy c = decodeCopy(r); if (std::getenv("COPIES")) std::printf("copy after draw %zu: xfb %d clear %d rgba %d,%d,%d,%d z %06x src %d,%d %dx%d fmt %d depth %d dst %08x pixelFmt %u\n", draws.size(), int(c.toXfb), int(c.clear), c.clearColor[0], c.clearColor[1], c.clearColor[2], c.clearColor[3], c.clearZ, c.srcX, c.srcY, c.srcW, c.srcH, c.fmt, int(c.depth), c.dstAddr, unsigned((r.bp[0x43]) & 7)); }
  void onTlutLoad(const GxRegs&) override { lastIsDraw = false; }
  const uint8_t* resolveDisplayList(uint32_t addr, uint32_t size) override { return mem->read(addr, size); }
};

static const int W = 640, H = 480;

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: %s replay.wwrp frameNo out_prefix\n", argv[0]); return 2; }
  const uint32_t want = uint32_t(std::atoi(argv[2]));
  FILE* in = std::fopen(argv[1], "rb");
  if (!in) { std::perror(argv[1]); return 1; }
  std::vector<uint8_t> f;
  { uint8_t b[1 << 16]; size_t n; while ((n = std::fread(b, 1, sizeof b, in)) > 0) f.insert(f.end(), b, b + n); std::fclose(in); }
  uint32_t hdr[3]; std::memcpy(hdr, &f[4], 12);
  Mem1 mem; mem.bytes.assign(hdr[1], 0);
  Sink sink; sink.mem = &mem;
  GxFifo fifo(&sink);
  sink.fifo = &fifo;
  fifo.setGuestArrayReader([&mem](uint32_t a, uint32_t n) { return mem.read(a, n); });
  fifo.setAuroraMode(hdr[0] == 2);   // version 2: a stream from Aurora (the native port), pc_wwrp_capture.cpp
  size_t at = 16;
  bool got = false;
  for (uint32_t i = 0; i < hdr[2] && !got; i++) {
    const uint8_t type = f[at]; at += 4;
    uint32_t a = 0, n = 0;
    if (type == 0) { std::memcpy(&n, &f[at], 4); at += 4; fifo.process(&f[at], n); at += n; }
    else if (type == 1) { std::memcpy(&a, &f[at], 4); std::memcpy(&n, &f[at + 4], 4); at += 8; std::memcpy(&mem.bytes[a], &f[at], n); at += n; }
    else { uint32_t fr; std::memcpy(&fr, &f[at], 4); at += 4; if (fr == want) got = true; else { sink.draws.clear(); sink.lastIsDraw = false; } }
  }
  if (!got) { std::fprintf(stderr, "frame %u not found\n", want); return 1; }

  if (const char* dd = std::getenv("DUMP_DRAW")) {
    const size_t k = size_t(std::atoi(dd));
    const GxState& S = sink.draws[k].st;
    {
      std::vector<GxDrawState> states; for (auto& x : sink.draws) states.push_back(x.st.draw);
      Options opt; opt.verifySamples = 0; opt.approxEffort = 0; opt.frameFogRange = frameFogRange(states.data(), states.size());
      FrameConfig fc; fc.efbW = 640; fc.efbH = 480; fc.resolve();
      const GxFog& F = S.draw.fog;
      std::printf("draw %zu: gx fog type %d start %g end %g near %g far %g rgb %d,%d,%d  projFar %g  proj ortho %d near %g far %g  frameFogRange %g\n", k, F.type, F.startZ, F.endZ, F.nearZ, F.farZ, F.r, F.g, F.b,
                  S.draw.projFar, int(S.proj.ortho), S.proj.nearZ, S.proj.farZ, opt.frameFogRange);
      const DrawPacket pk = buildDraw(S, sink.draws[k].d, opt, fc, nullptr);
      const PicaDraw& P = pk.pica();
      std::printf("  packet ok %d  pica fog en %d color %06x D %g scale %g offset %g sat %d maxErr %g note %s\n", int(pk.ok), int(P.fog.enable), P.fog.color, P.fog.depthRange,
                  P.fog.depthScale, P.fog.depthOffset, int(P.fog.depthSaturates), P.fog.maxErr255, P.fog.note.c_str());
      std::printf("  depthMap zbuf %d scale %g offset %g clear %06x; frame depth zbuf %d scale %g offset %g\n  lut:", int(P.depthMap.zBuffer), P.depthMap.scale, P.depthMap.offset, P.depthMap.clear,
                  int(pk.frame.depth.zBuffer), pk.frame.depth.scale, pk.frame.depth.offset);
      for (int i = 0; i < 128; i += 8) std::printf(" %d", int(P.fog.lut[i] >> 13));
      std::printf("\n");
      for (const std::string& n : pk.notes) std::printf("  note: %s\n", n.c_str());
      // depth range of the draw's vertices (eye w)
      VtxSetup setup = S.draw.vtx; setup.numPosMtx = S.draw.numPosMtx;
      float wmin = 1e30f, wmax = -1e30f; int behind = 0;
      const GxDraw& d = sink.draws[k].d;
      for (uint32_t i = 0; i < d.v.count; i++) {
        VtxInput vi; for (int c = 0; c < 3; c++) vi.pos[c] = d.v.pos[3 * i + c];
        if (d.v.hasPmIdx) vi.mtxIdx = d.v.pmIdx[i] / 3;
        const VtxOutput o = evalVertex(setup, S.uniforms, vi);
        wmin = std::min(wmin, o.clip[3]); wmax = std::max(wmax, o.clip[3]); behind += o.clip[3] <= 0;
      }
      std::printf("  blend: %s | alpha: %s | pixelFmt %d colorUpd %d alphaUpd %d dstAlphaEn %d dstAlpha %d | gx blend mode %d src %d dst %d logic %d\n", describe(S.draw.blend, P.blend).c_str(), describe(S.draw.alpha, P.alpha).c_str(), S.pixelFmt, int(S.colorUpdate), int(S.alphaUpdate), int(S.dstAlphaEnable), S.dstAlpha, S.draw.blend.mode, S.draw.blend.src, S.draw.blend.dst, S.draw.blend.logic);
      { std::map<int,int> pm; for (uint8_t v : sink.draws[k].d.v.pmIdx) pm[v]++; std::printf("  hasPmIdx %d numPosMtx %d curPosMtx %d pmIdx:", int(sink.draws[k].d.v.hasPmIdx), S.draw.numPosMtx, S.curPosMtx); for (auto& e : pm) std::printf(" %d(x%d)", e.first, e.second); std::printf("\n"); }
      { // in-front triangles by how far outside the screen their corners reach (NDC units; the screen is -1..1)
        const GxDraw& dd = sink.draws[k].d; VtxSetup su = S.draw.vtx; su.numPosMtx = S.draw.numPosMtx;
        std::vector<float> nx(dd.v.count), ny(dd.v.count), ww(dd.v.count), cz(dd.v.count);
        for (uint32_t i = 0; i < dd.v.count; i++) { VtxInput vi; for (int c = 0; c < 3; c++) vi.pos[c] = dd.v.pos[3*i+c]; if (dd.v.hasPmIdx) vi.mtxIdx = dd.v.pmIdx[i]/3;
          const VtxOutput o = evalVertex(su, S.uniforms, vi); ww[i] = o.clip[3]; cz[i] = o.clip[2]; nx[i] = o.clip[0]/o.clip[3]; ny[i] = o.clip[1]/o.clip[3]; }
        int bins[6] = {}; const float lim[5] = {1.05f, 2, 4, 16, 64};
        for (size_t t = 0; t + 2 < dd.tris.size(); t += 3) { float m = 0; bool front = true;
          for (int j = 0; j < 3; j++) { uint32_t v = dd.tris[t+j]; if (ww[v] <= 0) front = false; m = std::max({m, std::fabs(nx[v]), std::fabs(ny[v])}); }
          if (!front) continue; int b = 0; while (b < 5 && m > lim[b]) b++; bins[b]++; }
        std::printf("  in-front triangles by max |ndc|: <=1.05:%d <=2:%d <=4:%d <=16:%d <=64:%d >64:%d\n", bins[0], bins[1], bins[2], bins[3], bins[4], bins[5]);
      }
      if (std::getenv("VSCHECK")) {   // run the generated PICA vertex shader on the packet's own vertex buffer and compare with the reference
        PicaProgram2 prog; std::string perr;
        if (std::getenv("VSDUMP")) std::printf("%s\n---\n", P.vs.source.c_str());
        if (!parsePica(P.vs.source, prog, perr)) std::printf("  VS parse error: %s\n", perr.c_str());
        else {
          const GxDraw& dd = sink.draws[k].d; VtxSetup su = S.draw.vtx; su.numPosMtx = S.draw.numPosMtx;
          UniformMap U = pk.uniforms;
          for (auto it = U.begin(); it != U.end();) { if (!prog.uniform.count(it->first)) it = U.erase(it); else ++it; }
          for (const auto& u : prog.uniform) if (!U.count(u.first)) { std::printf("  MISSING uniform %s (%d regs)\n", u.first.c_str(), u.second.second); U[u.first] = std::vector<PVec>(size_t(u.second.second)); }
          double maxd[2] = {0, 0}; int shown = 0, bad[2] = {0, 0};
          for (uint32_t i = 0; i < pk.vertexCount; i++) {
            PVec in[16];
            for (int r = 0; r < 16; r++) in[r].v[3] = 1;
            for (const SubmitAttr& sa : pk.layout) {
              const uint8_t* src = &pk.vertices[size_t(pk.stride) * i + sa.offset];
              PVec& v = in[sa.reg]; v = PVec(); v.v[3] = 1;
              for (int c = 0; c < sa.components; c++) { if (sa.ubyte) v.v[c] = src[c]; else std::memcpy(&v.v[c], src + 4 * c, 4); }
            }
            VtxInput vi; for (int c = 0; c < 3; c++) vi.pos[c] = dd.v.pos[3*i+c]; if (dd.v.hasPmIdx) vi.mtxIdx = dd.v.pmIdx[i]/3;
            const VtxOutput o = evalVertex(su, S.uniforms, vi);
            float ref[4] = {0, 0, 0, 0};
            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) ref[r] += pk.frame.post[r][c] * o.clip[c];
            for (int f = 0; f < 2; f++) {
              std::map<std::string, PVec> out; std::string rerr;
              if (!runPica(prog, U, in, out, rerr, f == 1)) { std::printf("  VS run error: %s\n", rerr.c_str()); break; }
              const PVec& ps = out["position"];
              double d = 0; for (int c = 0; c < 4; c++) d = std::max(d, std::fabs(double(ps.v[c]) - ref[c]) / (std::fabs(double(ref[3])) + 1));
              maxd[f] = std::max(maxd[f], d); if (d > 0.01) bad[f]++;
              if (f == 1 && (shown < 4 || (d > 0.01 && shown < 8))) { shown++; std::printf("  v%u ref %.2f %.2f %.2f %.2f | pica(f24) %.2f %.2f %.2f %.2f\n", i, ref[0], ref[1], ref[2], ref[3], ps.v[0], ps.v[1], ps.v[2], ps.v[3]); }
            }
          }
          std::printf("  VS check: max rel diff f32 %.4g f24 %.4g; vertices off by >1%%: f32 %d f24 %d of %u\n", maxd[0], maxd[1], bad[0], bad[1], pk.vertexCount);
        }
      }
      { const GxDraw& dd = sink.draws[k].d; VtxSetup su = S.draw.vtx; su.numPosMtx = S.draw.numPosMtx; int ccw = 0, cw = 0, other = 0;
        std::vector<float> sx(dd.v.count), sy(dd.v.count), ww(dd.v.count);
        for (uint32_t i = 0; i < dd.v.count; i++) { VtxInput vi; for (int c = 0; c < 3; c++) vi.pos[c] = dd.v.pos[3*i+c]; if (dd.v.hasPmIdx) vi.mtxIdx = dd.v.pmIdx[i]/3;
          const VtxOutput o = evalVertex(su, S.uniforms, vi); ww[i] = o.clip[3]; sx[i] = o.clip[0]/o.clip[3]; sy[i] = o.clip[1]/o.clip[3]; }
        for (size_t t = 0; t + 2 < dd.tris.size(); t += 3) { const uint32_t a = dd.tris[t], b = dd.tris[t+1], c = dd.tris[t+2];
          if (ww[a] <= 0 || ww[b] <= 0 || ww[c] <= 0) { other++; continue; }
          const float ar = (sx[b]-sx[a])*(sy[c]-sy[a]) - (sx[c]-sx[a])*(sy[b]-sy[a]); if (ar > 0) ccw++; else cw++; }
        std::printf("  cull: gx cullMode %d -> pica %d mirrored %d | in-front triangles in GX NDC (y up): ccw %d cw %d (others %d) | viewport %g,%g %gx%g\n", S.cullMode, pk.frame.picaCull, int(pk.frame.mirrored), ccw, cw, other, S.viewport.x, S.viewport.y, S.viewport.width, S.viewport.height); }
      std::printf("  vertex w %g .. %g, %d of %u behind the camera\n", wmin, wmax, behind, d.v.count);
      for (int i = 0; i < 4; i++) std::printf("  tevReg[%d] %.3f %.3f %.3f %.3f   kcolor[%d] %.3f %.3f %.3f %.3f\n", i, S.tevReg[i].v[0], S.tevReg[i].v[1], S.tevReg[i].v[2], S.tevReg[i].v[3], i, S.kcolor[i].v[0], S.kcolor[i].v[1], S.kcolor[i].v[2], S.kcolor[i].v[3]);
      for (size_t si = 0; si < S.draw.tev.stages.size(); si++) { const TevStage& t = S.draw.tev.stages[si]; std::printf("  stage %zu kc kind %d fixed %g idx %d chan %d | ka kind %d idx %d chan %d\n", si, int(t.kc.kind), t.kc.fixed, t.kc.idx, t.kc.chan, int(t.ka.kind), t.ka.idx, t.ka.chan); }
      if (d.v.hasClr[0]) { int hist[4] = {}; for (uint32_t i = 0; i < d.v.count; i++) { int m = std::max({d.v.clr[0][4*i], d.v.clr[0][4*i+1], d.v.clr[0][4*i+2]}); hist[m / 64]++; }
        std::printf("  vertex colour max-channel histogram 0-63:%d 64-127:%d 128-191:%d 192-255:%d\n", hist[0], hist[1], hist[2], hist[3]); }
      for (const auto& u : pk.uniforms) if (u.first.find("tex") != std::string::npos) std::printf("  uniform %s (%zu vec)\n", u.first.c_str(), u.second.size());
      for (size_t si = 0; si < pk.texenv.size(); si++) std::printf("  texenv %zu: const %08x\n", si, pk.texenv[si].constColor);
    }
    for (int m = 0; m < 8; m++) {
      const GxTexBinding& b = S.tex[m];
      bool used = false;
      for (const TevStage& st : S.draw.tev.stages) used |= st.texmap == m;
      if (!b.valid || !used) continue;
      const size_t n = gxTextureBytes(b.fmt, b.width, b.height);
      const uint8_t* src = mem.read(b.addr, n);
      std::vector<uint8_t> rgba; std::string err;
      const bool ok = src && decodeGxTexture(b.fmt, b.width, b.height, src, n, nullptr, rgba, err);
      size_t nz = 0; if (src) for (size_t i = 0; i < n; i++) nz += src[i] != 0;
      std::printf("draw %zu texmap %d: addr %08x %dx%d fmt %d mips %d wrap %d/%d bytes %zu nonzero %zu decode %s %s\n", k, m, b.addr, b.width, b.height, b.fmt, b.mipLevels(), b.wrapS, b.wrapT, n, nz, ok ? "ok" : "FAILED", err.c_str());
      if (!ok) continue;
      auto ppm = [&](const std::string& path, const std::vector<uint8_t>& px, int w, int h) {
        FILE* o = std::fopen(path.c_str(), "wb"); std::fprintf(o, "P6 %d %d 255\n", w, h);
        for (int i = 0; i < w * h; i++) std::fwrite(&px[4 * size_t(i)], 1, 3, o);
        std::fclose(o);
      };
      ppm(std::string(argv[3]) + "_tex" + std::to_string(m) + "_gc.ppm", rgba, b.width, b.height);
      const PicaTexture pt = chooseFormat(rgba, b.width, b.height);
      std::printf("   pica %s %dx%d exact %d mean %.2f max %.2f %s\n", picaTexFmtName(pt.fmt), pt.width, pt.height, int(pt.exact), pt.meanErr255, pt.maxErr255, pt.note.c_str());
      ppm(std::string(argv[3]) + "_tex" + std::to_string(m) + "_pica.ppm", picaDecodeImage(pt.fmt, pt.data.data(), pt.width, pt.height), pt.width, pt.height);
    }
    return 0;
  }
  std::vector<int32_t> ids(size_t(W) * H, -1);
  size_t pixWatch = ~size_t(0), lastPix = ~size_t(0);
  if (const char* pw = std::getenv("PIXEL")) { int px = 0, py = 0; std::sscanf(pw, "%d,%d", &px, &py); pixWatch = size_t(py) * W + px; }
  std::vector<float> zb(size_t(W) * H, 1e30f);
  std::vector<size_t> cover(sink.draws.size(), 0);
  const size_t lim = argc > 4 ? size_t(std::atoi(argv[4])) : sink.draws.size();
  for (size_t k = 0; k < sink.draws.size() && k < lim; k++) {
    const GxState& S = sink.draws[k].st;
    const GxDraw& d = sink.draws[k].d;
    if (d.tris.empty()) continue;
    if (std::getenv("DEPTH_ONLY") && !S.draw.z.update) continue;
    VtxSetup setup = S.draw.vtx;
    setup.numPosMtx = S.draw.numPosMtx;
    const GxViewport& vp = S.viewport;
    std::vector<float> sx(d.v.count), sy(d.v.count), sz(d.v.count);
    std::vector<bool> ok(d.v.count);
    for (uint32_t i = 0; i < d.v.count; i++) {
      VtxInput vi;
      for (int c = 0; c < 3; c++) vi.pos[c] = d.v.pos[3 * i + c];
      if (d.v.hasPmIdx) vi.mtxIdx = d.v.pmIdx[i] / 3;
      const VtxOutput o = evalVertex(setup, S.uniforms, vi);
      ok[i] = o.clip[3] > 1e-6f;
      if (!ok[i]) continue;
      const float nx = o.clip[0] / o.clip[3], ny = o.clip[1] / o.clip[3];
      sx[i] = vp.x + vp.width * 0.5f * (1 + nx);
      sy[i] = vp.y + vp.height * 0.5f * (1 - ny);
      sz[i] = o.clip[2] / o.clip[3];
    }
    for (size_t t = 0; t + 2 < d.tris.size(); t += 3) {
      const uint32_t a = d.tris[t], b = d.tris[t + 1], c = d.tris[t + 2];
      if (!ok[a] || !ok[b] || !ok[c]) continue;
      const float x0 = std::max(0.f, std::floor(std::min({sx[a], sx[b], sx[c]}))), x1 = std::min(float(W - 1), std::ceil(std::max({sx[a], sx[b], sx[c]})));
      const float y0 = std::max(0.f, std::floor(std::min({sy[a], sy[b], sy[c]}))), y1 = std::min(float(H - 1), std::ceil(std::max({sy[a], sy[b], sy[c]})));
      const float area = (sx[b] - sx[a]) * (sy[c] - sy[a]) - (sx[c] - sx[a]) * (sy[b] - sy[a]);
      if (std::fabs(area) < 1e-9f) continue;
      for (int y = int(y0); y <= int(y1); y++)
        for (int x = int(x0); x <= int(x1); x++) {
          const float px = x + 0.5f, py = y + 0.5f;
          const float w0 = ((sx[b] - px) * (sy[c] - py) - (sx[c] - px) * (sy[b] - py)) / area;
          const float w1 = ((sx[c] - px) * (sy[a] - py) - (sx[a] - px) * (sy[c] - py)) / area;
          const float w2 = 1 - w0 - w1;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue;
          const float z = w0 * sz[a] + w1 * sz[b] + w2 * sz[c];
          const size_t p = size_t(y) * W + x;
          if (S.draw.z.compare && z > zb[p]) continue;
          if (S.draw.z.compare && S.draw.z.update) zb[p] = z;
          ids[p] = int32_t(k);
          if (pixWatch == p && lastPix != k) { lastPix = k; std::printf("pixel hit: draw %zu z=%g\n", k, z); }
        }
    }
  }
  for (int32_t v : ids) if (v >= 0) cover[size_t(v)]++;

  const std::string pre = argv[3];
  FILE* o = std::fopen((pre + ".ids").c_str(), "wb");
  std::fwrite(ids.data(), 4, ids.size(), o);
  std::fclose(o);
  FILE* t = std::fopen((pre + ".txt").c_str(), "w");
  for (size_t k = 0; k < sink.draws.size(); k++) {
    const GxState& S = sink.draws[k].st;
    const GxDraw& d = sink.draws[k].d;
    std::fprintf(t, "%zu\tmerged=%d verts=%u cover=%zu\tchans=%d texgens=%d stages=%d ind=%d\tc0 en=%d mat=%d amb=%d lm=%02x a0 en=%d mat=%d\tvtxclr=%d nrm=%d\tmat=%g,%g,%g,%g amb=%g,%g,%g,%g\tz=%d/%d blend=%d",
                 k, sink.draws[k].merged, d.v.count, cover[k], S.numChans, S.numTexGens, S.numTevStages, S.numIndStages, S.draw.vtx.color0.enable, S.draw.vtx.color0.matSrc,
                 S.draw.vtx.color0.ambSrc, S.draw.vtx.color0.lightMask, S.draw.vtx.alpha0.enable, S.draw.vtx.alpha0.matSrc, int(d.v.hasClr[0]), int(d.v.hasNrm),
                 S.uniforms.matColor[0], S.uniforms.matColor[1], S.uniforms.matColor[2], S.uniforms.matColor[3], S.uniforms.ambColor[0], S.uniforms.ambColor[1], S.uniforms.ambColor[2],
                 S.uniforms.ambColor[3], int(S.draw.z.compare), int(S.draw.z.update), int(S.draw.blend.mode));
    std::fprintf(t, "\tbl=%d,%d,%d pf=%d cu=%d au=%d da=%d/%d", S.draw.blend.mode, S.draw.blend.src, S.draw.blend.dst, S.pixelFmt, int(S.colorUpdate), int(S.alphaUpdate), int(S.dstAlphaEnable), S.dstAlpha);
    for (int m = 0; m < 8; m++) if (S.tex[m].valid) std::fprintf(t, "\ttex%d=%08x %dx%d f%d", m, S.tex[m].addr, S.tex[m].width, S.tex[m].height, S.tex[m].fmt);
    for (size_t s = 0; s < S.draw.tev.stages.size(); s++) {
      const TevStage& st = S.draw.tev.stages[s];
      std::fprintf(t, "\ts%zu[tm%d tc%d ras%d c=%d,%d,%d,%d a=%d,%d,%d,%d]", s, st.texmap, st.texcoord, st.rasChan, int(st.c.in[0]), int(st.c.in[1]), int(st.c.in[2]), int(st.c.in[3]),
                   int(st.a.in[0]), int(st.a.in[1]), int(st.a.in[2]), int(st.a.in[3]));
    }
    for (const std::string& n : S.notes) std::fprintf(t, "\tNOTE %s", n.c_str());
    std::fprintf(t, "\n");
  }
  std::fclose(t);
  std::printf("frame %u: %zu draws -> %s.ids / .txt\n", want, sink.draws.size(), pre.c_str());
  return 0;
}
