#include "draw.h"
#include "approx.h"
#include "multipass.h"
#include "picaasm.h"

#include <algorithm>
#include <cstdio>

namespace t2p {
namespace {

// A texgen can produce q != 1 (only texture unit 0 can project on the PICA). Matches the generator's own q==1 test
// (MTX2x4, or MTX3x4 from a texcoord attribute with the identity matrix).
bool maybeProjective(const VtxTexGen& g) {
  if (g.type == 20) return true;   // SCREEN
  if (g.type != 0) return false;
  const bool texSrc = g.src >= 4 && g.src <= 11;
  return !(texSrc && g.mtx == 60);
}

struct UnitPair { int texmap, texcoord; };

uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

std::string num(int v) { return std::to_string(v); }

}  // namespace

// (Claude v105 diag) compile phase ticks: vs gen 1, vs gen 2, parse, tev, fixed (set g_compileClock to enable)
uint64_t (*g_compileClock)() = nullptr;
uint64_t g_compilePhase[8] = {};

PicaDraw compileDrawSingle(const GxDrawState& g, const Options& opt) {
  PicaDraw D;
  auto block = [&](const std::string& s) { D.blockers.push_back(s); };
  bool approx = false;
  auto note = [&](const std::string& s) { D.notes.push_back(s); approx = true; };

  for (const std::string& s : g.importBlockers) block(s);

  // ---------------------------------------------------------------- which stages read what; texture unit assignment
  TevMaterial tev = g.tev;
  bool needColor = false;
  std::vector<UnitPair> pairs;
  std::vector<bool> readsTex(tev.stages.size(), false);
  for (size_t i = 0; i < tev.stages.size(); i++) {
    const TevStage& st = tev.stages[i];
    bool rt = false, rr = false;
    for (const TevChan* ch : {&st.c, &st.a})
      for (Leaf l : ch->in) {
        if (l == L_TEXC || l == L_TEXA) rt = true;
        if (l == L_RASC || l == L_RASA) rr = true;
      }
    readsTex[i] = rt;
    if (rr) needColor = true;
    if (rt && st.texmap >= 0) {
      if (st.texcoord < 0) { block("stage " + num(int(i)) + " reads texmap " + num(st.texmap) + " but has no texcoord"); continue; }
      if (st.texcoord >= int(g.vtx.texgen.size())) { block("stage " + num(int(i)) + ": texcoord " + num(st.texcoord) + " has no texgen"); continue; }
      const bool dup = std::any_of(pairs.begin(), pairs.end(), [&](const UnitPair& p) { return p.texmap == st.texmap && p.texcoord == st.texcoord; });
      if (!dup) pairs.push_back({st.texmap, st.texcoord});
    }
  }
  // unit 0 is the only projective one: give it to a texgen that may produce q != 1
  std::stable_sort(pairs.begin(), pairs.end(), [&](const UnitPair& a, const UnitPair& b) {
    return maybeProjective(g.vtx.texgen[a.texcoord]) && !maybeProjective(g.vtx.texgen[b.texcoord]);
  });
  if (pairs.size() > 3) block(num(int(pairs.size())) + " distinct (texmap, texcoord) pairs: the PICA has 3 texture units (needs multipass)");
  D.numUnits = int(std::min<size_t>(pairs.size(), 3));
  for (int u = 0; u < D.numUnits; u++) { D.unitTexmap[u] = pairs[u].texmap; D.unitTexcoord[u] = pairs[u].texcoord; }
  for (int u = 0; u < D.numUnits; u++)
    for (int v = u + 1; v < D.numUnits; v++)
      if (D.unitTexmap[u] == D.unitTexmap[v]) {
        D.info.push_back("GX texmap " + num(D.unitTexmap[u]) + " is bound to PICA units " + num(u) + " and " + num(v) + " (different texcoords)");
      }
  for (size_t i = 0; i < tev.stages.size(); i++) {
    TevStage& st = tev.stages[i];
    int unit = -1;
    if (readsTex[i] && st.texmap >= 0)
      for (int u = 0; u < D.numUnits; u++)
        if (D.unitTexmap[u] == st.texmap && D.unitTexcoord[u] == st.texcoord) unit = u;
    if (readsTex[i] && st.texmap >= 0 && unit < 0 && D.blockers.empty()) block("stage " + num(int(i)) + ": no texture unit could be assigned");
    st.texmap = unit >= 0 ? unit : (readsTex[i] ? 99 : -1);   // 99: the translator reports "texmap outside 0..2" for an unassigned read
    if (unit >= 0)
      for (const TevChan* ch : {&st.c, &st.a})
        for (Leaf l : ch->in)
          if (l == L_TEXA) D.unitNeedAlpha[unit] = true;
  }

  // ---------------------------------------------------------------- vertex pipeline
  VtxSetup vs = g.vtx;
  vs.needColor = needColor;
  vs.numPosMtx = g.numPosMtx;
  vs.flipT = opt.flipTexT;
  vs.texgen.clear();
  vs.texScaleMask = 0;
  for (int u = 0; u < D.numUnits; u++) {
    vs.texgen.push_back(g.vtx.texgen[D.unitTexcoord[u]]);
    if (D.unitTexmap[u] >= 0 && D.unitTexmap[u] < 8 && (g.texmapScaled >> D.unitTexmap[u]) & 1) vs.texScaleMask |= uint8_t(1u << u);
  }
  vs.dbgClipReg = -1;
  vs.dbgTc = -1;
  // pass 1: every attribute gets a provisional register; pass 2 packs the registers that the shader really declares
  {
    vs.inPos = 0; vs.inNrm = 1; vs.inClr0 = 2; vs.inMtxIdx = 3;
    for (int t = 0; t < 8; t++) vs.inTex[t] = 4 + t;
    uint64_t c0 = g_compileClock ? g_compileClock() : 0;
    VtxShader first = genVertexShader(vs);
    if (g_compileClock) { const uint64_t c = g_compileClock(); g_compilePhase[0] += c - c0; c0 = c; }
    if (!first.ok) {
      block("vertex pipeline: " + first.error);
    } else {
      int reg = 0;
      auto add = [&](DrawAttr::Kind k, int idx) { D.attrs.push_back({k, idx, reg++}); return reg - 1; };
      VtxSetup packed = vs;
      packed.inPos = add(DrawAttr::Pos, 0);
      packed.inNrm = first.usesNrmAttr ? add(DrawAttr::Nrm, 0) : -1;
      packed.inClr0 = first.usesClrAttr ? add(DrawAttr::Clr0, 0) : -1;
      packed.inMtxIdx = first.usesMtxIdxAttr ? add(DrawAttr::MtxIdx, 0) : -1;
      for (int t = 0; t < 8; t++) packed.inTex[t] = first.usesTexAttr[t] ? add(DrawAttr::Tex, t) : -1;
      D.vtx = packed;
      D.vs = genVertexShader(packed);
      if (g_compileClock) { const uint64_t c = g_compileClock(); g_compilePhase[1] += c - c0; c0 = c; }
      if (!D.vs.ok) block("vertex pipeline (packed): " + D.vs.error);
      else {
        for (const std::string& n : D.vs.notes) note("vertex: " + n);
        D.vsKey = fnv1a(D.vs.source);
        // (Claude v105) memo by source (hash + length + full compare): the assembly check is ~0.5 ms on the ARM11 and many draws share a shader
        struct ParseMemo { uint64_t key = 0; std::string src; bool ok = false; std::string err; int ins = 0, opd = 0, regs = 0; };
        static thread_local ParseMemo pmemo[32];
        static thread_local unsigned pnext = 0;
        ParseMemo* hit = nullptr;
        for (ParseMemo& m : pmemo) if (m.key == D.vsKey && m.src == D.vs.source) { hit = &m; break; }
        if (!hit) {
          hit = &pmemo[pnext++ & 31];
          PicaProgram2 P;   // model of picasso: operand classes, descriptor / instruction / uniform budgets
          hit->err.clear();
          hit->ok = parsePica(D.vs.source, P, hit->err);
          if (hit->ok) { hit->ins = int(P.ins.size()); hit->opd = P.opdescs; hit->regs = P.fvecRegs + P.constRegs; }
          hit->key = D.vsKey; hit->src = D.vs.source;
        }
        if (!hit->ok) block("vertex shader does not assemble: " + hit->err);
        else { D.vsInstructions = hit->ins; D.vsOpdescs = hit->opd; D.vsUniformRegs = hit->regs; }
        if (g_compileClock) { const uint64_t c = g_compileClock(); g_compilePhase[2] += c - c0; c0 = c; }
      }
    }
  }

  // ---------------------------------------------------------------- texenv program
  uint64_t ct0 = g_compileClock ? g_compileClock() : 0;
  if (D.blockers.empty()) {
    D.tev = translate(tev, opt);
    if (D.tev.overall == Fidelity::Unsupported) {
      bool any = false;
      for (const StageNote& n : D.tev.notes)
        if (n.color == Fidelity::Unsupported || n.alpha == Fidelity::Unsupported || n.gcStage < 0) { block("TEV: " + n.text); any = true; }
      if (!any) block("TEV: unsupported");
    } else if (D.tev.overall == Fidelity::Approx) {
      for (const StageNote& n : D.tev.notes)
        if (n.color == Fidelity::Approx || n.alpha == Fidelity::Approx) note("TEV stage " + num(n.gcStage) + ": " + n.text);
      if (D.tev.notes.empty()) note("TEV: approximate");
    }
    if (g.indirect) note("indirect texture warp dropped: the base texture is drawn unwarped");
  }

  if (g_compileClock) { const uint64_t c = g_compileClock(); g_compilePhase[3] += c - ct0; ct0 = c; }
  // ---------------------------------------------------------------- fixed-function state
  D.alpha = alphaFromGx(g.alpha);
  if (!D.alpha.exact) note("alpha test: " + D.alpha.note);
  D.blend = blendFromGx(g.blend);
  if (!D.blend.exact) note("blend: " + D.blend.note);
  if (D.blend.needsDstAlpha) D.info.push_back("blend reads destination alpha: the target needs an RGBA buffer cleared the way the GC did");
  D.depth = depthFromGx(g.z);
  const uint64_t cf0 = g_compileClock ? g_compileClock() : 0;
  D.fog = opt.fogCompileCache ? opt.fogCompileCache->get(g.fog,g.projFar,true,opt.frameFogRange) : fogFromGx(g.fog, g.projFar, true, opt.frameFogRange);
  if (g_compileClock) g_compilePhase[6] += g_compileClock() - cf0;
  if (!D.fog.exact) note("fog: " + D.fog.note);
  if (g_picaForceZDepth) {   // (Claude v93) experiment: Z map everywhere, no fog (pipeline.h)
    if (D.fog.enable) D.info.push_back("fog dropped (depth_z experiment)");
    D.fog.enable = false;
    D.depthMap = depthMapZ();
  } else if (D.fog.enable) {
    D.depthMap = depthMapW(D.fog.depthRange);
    if (opt.frameFogRange > 0) D.info.push_back("fog: W-buffer depth map, frame range D = " + num(int(D.fog.depthRange)));
    else D.info.push_back("fog: W-buffer depth map (D = " + num(int(D.fog.depthRange)) + "); every draw of the frame must use it (compile the frame with frameFogRange)");
  } else if (opt.frameFogRange > 0) {
    D.depthMap = depthMapW(opt.frameFogRange);   // no fog on this draw, but the frame's depth buffer is a W buffer
    D.info.push_back("frame uses fog: W-buffer depth map, frame range D = " + num(int(opt.frameFogRange)));
  } else {
    D.depthMap = depthMapZ();
  }
  if (D.depthMap.zBuffer && picaReverseZ()) D.depth.func = picaMirrorDepthFunc(D.depth.func);   // (Claude v107) reversed Z
  if (D.vs.ok && D.vs.tc0Projective) D.info.push_back("texture unit 0 runs in projective mode (texcoord0w)");

  D.overall = !D.blockers.empty() ? Fidelity::Unsupported : (approx ? Fidelity::Approx : Fidelity::Exact);
  if (g_compileClock) g_compilePhase[4] += g_compileClock() - ct0;
  return D;
}

PicaDraw compileDrawExact(const GxDrawState& g, const Options& opt) {
  PicaDraw D = compileDrawSingle(g, opt);
  if (D.overall != Fidelity::Unsupported || !opt.multipass) return D;
  PicaDraw M;
  std::string why;
  if (compileSplit(g, opt, M, why)) return M;
  D.blockers.push_back("multipass: " + why);
  return D;
}

PicaDraw compileDraw(const GxDrawState& g, const Options& opt) {
  PicaDraw D = compileDrawExact(g, opt);
  if (D.overall != Fidelity::Unsupported || !opt.approximate) return D;
  PicaDraw A;
  std::string why;
  if (compileApprox(g, opt, D, A, why)) return A;
  D.blockers.push_back("approximate rung: " + why);
  return D;
}

void FrameFogRange::add(const GxDrawState& draw) {
  if(!draw.projOrtho)far_=std::max(far_,draw.projFar);
  const GxFog& f=draw.fog;
  if(f.type==0||(f.type&8))return;
  for(const auto& x:seen_)
    if(x.projFar==draw.projFar && x.f.type==f.type && x.f.startZ==f.startZ && x.f.endZ==f.endZ && x.f.nearZ==f.nearZ && x.f.farZ==f.farZ)return;
  seen_.push_back({f,draw.projFar});
  range_=std::max(range_,fogDepthRange(f,draw.projFar));
}

float frameFogRange(const GxDrawState* draws, size_t n) {
  // fogFromGx builds the whole LUT (and measures its error) to find a draw's range: a frame has hundreds of fogged draws but only a handful of distinct
  // fog settings, so each distinct (fog, projFar) is evaluated once
  struct Seen { GxFog f; float projFar; };
  std::vector<Seen> seen;
  float D = 0, far = 0;
  for (size_t i = 0; i < n; i++) {
    if (!draws[i].projOrtho) far = std::max(far, draws[i].projFar);   // (Claude v84) see FrameFogRange::value
    const GxFog& f = draws[i].fog;
    if (f.type == 0 || (f.type & 8)) continue;                       // no fog / orthographic fog (not translated)
    bool dup = false;
    for (const Seen& x : seen)
      if (x.projFar == draws[i].projFar && x.f.type == f.type && x.f.startZ == f.startZ && x.f.endZ == f.endZ && x.f.nearZ == f.nearZ && x.f.farZ == f.farZ) { dup = true; break; }
    if (dup) continue;                                               // (the colour does not change the range)
    seen.push_back({f, draws[i].projFar});
    const PicaFog pf = fogFromGx(f, draws[i].projFar);
    if (pf.enable) D = std::max(D, pf.depthRange);
  }
  return D > 0 ? std::max(D, far) : 0.0f;
}

}  // namespace t2p
