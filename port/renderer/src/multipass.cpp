#include "multipass.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>

namespace t2p {
namespace {

// registers: 0 = PREV, 1..3 = REG0..2; channel 0 = colour, 1 = alpha
int regOfLeaf(Leaf l, int& chan) {
  switch (l) {
    case L_CPREV: chan = 0; return 0;
    case L_APREV: chan = 1; return 0;
    case L_C0: chan = 0; return 1;
    case L_A0: chan = 1; return 1;
    case L_C1: chan = 0; return 2;
    case L_A1: chan = 1; return 2;
    case L_C2: chan = 0; return 3;
    case L_A2: chan = 1; return 3;
    default: return -1;
  }
}

std::string num(int v) { return std::to_string(v); }

}  // namespace

std::vector<std::pair<int, int>> crossingRegs(const TevMaterial& t, int c) {
  bool written[4][2] = {};
  for (int i = 0; i < c && i < int(t.stages.size()); i++) {
    written[t.stages[size_t(i)].c.out & 3][0] = true;
    written[t.stages[size_t(i)].a.out & 3][1] = true;
  }
  bool wr2[4][2] = {};
  std::set<std::pair<int, int>> cross;
  for (size_t i = size_t(c); i < t.stages.size(); i++) {
    const TevStage& st = t.stages[i];
    for (const TevChan* ch : {&st.c, &st.a})
      for (Leaf l : ch->in) {
        int chn = 0;
        const int r = regOfLeaf(l, chn);
        if (r >= 0 && !wr2[r][chn] && written[r][chn]) cross.insert({r, chn});
      }
    wr2[st.c.out & 3][0] = true;
    wr2[st.a.out & 3][1] = true;
  }
  return std::vector<std::pair<int, int>>(cross.begin(), cross.end());
}

namespace {

// A TEV register that is written before the cut and read after it cannot cross through the scratch (it holds PREV only). When its value does not depend on Previous, it is rebuilt in
// the second pass instead: the writing stages (and the earlier writers of the registers they read) are copied after the scratch read, each with only the channel that is needed
// writing its register and the other channel passing Previous through. The copies see the same fragment (same texcoords, colours, textures, constants), so they compute the same value.
// `need`: (register 1..3, channel) pairs that cross. false: why (a writer reads Previous).
bool rematerialize(const TevMaterial& t, int cut, const std::set<std::pair<int, int>>& need, std::vector<TevStage>& out, std::string& why) {
  std::set<std::pair<int, int>> dup;   // (stage, channel) to copy
  auto lastWriter = [&](int reg, int chn, int before) {
    for (int i = before - 1; i >= 0; i--) if (((chn == 0 ? t.stages[size_t(i)].c.out : t.stages[size_t(i)].a.out) & 3) == reg) return i;
    return -1;
  };
  std::vector<std::pair<int, int>> work;
  for (const auto& rc : need) { const int w = lastWriter(rc.first, rc.second, cut); if (w >= 0) work.push_back({w, rc.second}); }
  while (!work.empty()) {
    const auto sc = work.back();
    work.pop_back();
    if (dup.count(sc)) continue;
    dup.insert(sc);
    const TevStage& st = t.stages[size_t(sc.first)];
    const TevChan& ch = sc.second == 0 ? st.c : st.a;
    for (Leaf l : ch.in) {   // (an alpha R8 compare carries the colour channel's inputs a and b in its own in[0], in[1])
      int chn = 0;
      const int r = regOfLeaf(l, chn);
      if (r == 0) { why = "the value of TEV register depends on Previous (stage " + num(sc.first) + ")"; return false; }
      if (r < 0) continue;
      const int w2 = lastWriter(r, chn, sc.first);
      if (w2 >= 0) work.push_back({w2, chn});   // an earlier writer of a register this one reads; with none, the register still has its initial value, the same in both passes
    }
  }
  std::set<int> stages;
  for (const auto& sc : dup) stages.insert(sc.first);
  for (int i : stages) {
    TevStage c = t.stages[size_t(i)];
    for (int chn = 0; chn < 2; chn++) {
      if (dup.count({i, chn})) continue;
      TevChan& x = chn == 0 ? c.c : c.a;
      TevChan pass;
      pass.in[3] = chn == 0 ? L_CPREV : L_APREV;
      x = pass;
    }
    out.push_back(c);
  }
  return true;
}

}  // namespace

bool makeSplit(const GxDrawState& g, int cut, SplitDraws& out, std::string& why) {
  const int n = int(g.tev.stages.size());
  if (cut < 1 || cut >= n) { why = "no stage on one side of the cut"; return false; }
  std::set<std::pair<int, int>> need;
  for (const auto& rc : crossingRegs(g.tev, cut)) if (rc.first != 0) need.insert(rc);
  std::vector<TevStage> prologue;
  if (!need.empty()) {
    std::string rw;
    if (!rematerialize(g.tev, cut, need, prologue, rw)) {
      why = "TEV register " + num(need.begin()->first - 1) + " is written before stage " + num(cut) + " and read after it (" + rw + ")";
      return false;
    }
  }
  if (g.vtx.texgen.size() >= 3 + 5) { why = "no texcoord slot left for the scratch read"; return false; }

  SplitDraws S;
  // ---- the draw state's behaviour under two passes
  const PicaBlend pb = blendFromGx(g.blend);
  const PicaAlphaTest pa = alphaFromGx(g.alpha);
  const bool blending = pb.blendsAnything;   // set by blendFromGx for every real blend and for every logic op but COPY
  const bool orderedFn = g.z.compare && (g.z.func == 1 || g.z.func == 3 || g.z.func == 4 || g.z.func == 6);
  std::vector<std::string> reasons;
  if (blending) reasons.push_back("it blends");
  if (pa.enable) reasons.push_back("it has an alpha test");
  if (g.z.compare && g.z.func != 7) {
    if (!orderedFn) reasons.push_back(g.z.func == 0 ? "its depth test never passes" : "its depth function is EQUAL / NOTEQUAL");
    else if (!g.z.update) reasons.push_back("it does not write depth");
  }
  S.exactFragments = reasons.empty();
  for (size_t i = 0; i < reasons.size(); i++) S.fragmentNote += (i ? ", " : "") + reasons[i];
  S.depthOrdered = orderedFn && g.z.update;
  S.depthFar = !(g.z.func == 4 || g.z.func == 6);

  // ---- first pass: stages before the cut, no blending / alpha test / fog, depth on the scratch's own buffer
  S.first = g;
  S.first.tev.stages.assign(g.tev.stages.begin(), g.tev.stages.begin() + cut);
  S.first.alpha = GxAlphaCompare{};
  S.first.blend = GxBlend{};
  S.first.fog = GxFog{};
  S.first.z.compare = S.depthOrdered;
  S.first.z.func = S.depthOrdered ? g.z.func : uint8_t(7);
  S.first.z.update = S.depthOrdered;

  // ---- second pass: the scratch read, then the rest of the material with the draw's own state
  S.second = g;
  S.second.tev.stages.assign(g.tev.stages.begin() + cut, g.tev.stages.end());
  S.second.vtx.texgen.push_back(VtxTexGen{20, 0, 0, false, 125});
  TevStage rd;
  rd.texmap = kScratchTexmap;
  rd.texcoord = int(S.second.vtx.texgen.size()) - 1;
  rd.c.in[0] = rd.c.in[1] = rd.c.in[2] = L_ZERO; rd.c.in[3] = L_TEXC;
  rd.a.in[0] = rd.a.in[1] = rd.a.in[2] = L_ZERO; rd.a.in[3] = L_TEXA;
  rd.c.clamp = rd.a.clamp = true;
  S.second.tev.stages.insert(S.second.tev.stages.begin(), rd);
  // registers rebuilt for the second pass go right after the scratch read (which must come first: it takes texture unit 0)
  S.second.tev.stages.insert(S.second.tev.stages.begin() + 1, prologue.begin(), prologue.end());
  if (!need.empty()) {
    S.rematNote = "TEV register";
    for (const auto& rc : need) S.rematNote += " " + num(rc.first - 1) + (rc.second ? ".a" : "");
    S.rematNote += " is rebuilt in the second pass (" + num(int(prologue.size())) + " stage" + (prologue.size() == 1 ? "" : "s") + " copied)";
  }
  out = std::move(S);
  return true;
}

bool compileSplit(const GxDrawState& g, const Options& opt, PicaDraw& out, std::string& why) {
  const int n = int(g.tev.stages.size());
  if (n < 2) { why = "a one-stage material has nowhere to cut"; return false; }
  std::vector<int> cuts;
  for (int c = 1; c < n; c++) cuts.push_back(c);
  std::stable_sort(cuts.begin(), cuts.end(), [&](int a, int b) { return std::abs(2 * a - n) < std::abs(2 * b - n); });

  Options sub = opt;
  sub.multipass = false;
  std::string firstWhy;
  int bestRank = -1, bestCost = 1 << 30;
  PicaDraw bestA, bestB;
  SplitDraws bestS;
  int bestCut = 0;
  for (int c : cuts) {
    SplitDraws S;
    std::string w;
    if (!makeSplit(g, c, S, w)) { if (firstWhy.empty()) firstWhy = w; continue; }
    PicaDraw A = compileDrawSingle(S.first, sub);
    if (A.overall == Fidelity::Unsupported) { if (firstWhy.empty()) firstWhy = "first pass: " + (A.blockers.empty() ? std::string("unsupported") : A.blockers[0]); continue; }
    PicaDraw B = compileDrawSingle(S.second, sub);
    if (B.overall == Fidelity::Unsupported) { if (firstWhy.empty()) firstWhy = "second pass: " + (B.blockers.empty() ? std::string("unsupported") : B.blockers[0]); continue; }
    if (B.numUnits < 1 || B.unitTexmap[0] != kScratchTexmap) { if (firstWhy.empty()) firstWhy = "the scratch read did not get texture unit 0"; continue; }
    const int rank = int(std::min(A.overall, B.overall));   // 2 Exact, 1 Approx
    const int cost = int(A.tev.prog.stages.size() + B.tev.prog.stages.size());
    if (rank > bestRank || (rank == bestRank && cost < bestCost)) {
      bestRank = rank; bestCost = cost; bestA = std::move(A); bestB = std::move(B); bestS = std::move(S); bestCut = c;
    }
    if (rank == int(Fidelity::Exact)) break;
  }
  if (bestRank < 0) { why = firstWhy.empty() ? "no cut compiles" : firstWhy; return false; }

  PicaDraw& A = bestA;
  PicaDraw& B = bestB;
  B.role = PassRole::ScratchRead;
  A.role = PassRole::ScratchWrite;
  A.splitAt = bestCut;
  A.scratchDepth = bestS.depthOrdered;
  A.scratchDepthFar = bestS.depthFar;
  // the notes of both passes, once each
  std::vector<std::string> notes = A.notes;
  for (const std::string& s : B.notes) if (std::find(notes.begin(), notes.end(), s) == notes.end()) notes.push_back(s);
  std::vector<std::string> info = A.info;
  for (const std::string& s : B.info) if (std::find(info.begin(), info.end(), s) == info.end()) info.push_back(s);
  bool approx = A.overall == Fidelity::Approx || B.overall == Fidelity::Approx;
  if (!bestS.exactFragments) {
    notes.push_back("two passes (cut after GX stage " + num(bestCut - 1) + "): where triangles of this draw overlap on screen a fragment may read another fragment's first-pass colour, because " + bestS.fragmentNote);
    approx = true;
  }
  if (!bestS.rematNote.empty()) info.push_back("multipass: " + bestS.rematNote);
  info.push_back("multipass: GX stages 0.." + num(bestCut - 1) + " render into the scratch target" + (bestS.depthOrdered ? " (depth-tested on its own buffer)" : "") +
                 ", stages " + num(bestCut) + ".." + num(n - 1) + " run in a second pass that reads it back");
  // the second pass carries the draw's fixed-function state; its depth map stands for the draw (the first pass only needs one that orders its fragments)
  A.notes = notes; B.notes = notes;
  A.info = info; B.info = info;
  A.overall = approx ? Fidelity::Approx : Fidelity::Exact;
  B.overall = A.overall;
  A.pass2 = std::make_shared<const PicaDraw>(std::move(B));
  out = std::move(A);
  return true;
}

}  // namespace t2p
