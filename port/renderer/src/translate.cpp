#include "translate.h"
#include "bundle.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <chrono>

#include "search.h"

namespace t2p {

const char* fidelityName(Fidelity f) {
  switch (f) {
    case Fidelity::Exact: return "EXACT";
    case Fidelity::Approx: return "APPROX";
    default: return "UNSUPPORTED";
  }
}

// ------------------------------------------------------------------ rng
float Rng::v8() {
  float r = f();
  if (r < 0.12f) return 0.0f;
  if (r > 0.88f) return 1.0f;
  return std::floor(f() * 255.0f + 0.5f) / 255.0f;
}
Rgba Rng::rgba() { Rgba r; for (float& x : r.v) x = v8(); return r; }
Inputs Rng::inputs() {
  Inputs in;
  for (Rgba& t : in.tex) t = rgba();
  for (Rgba& t : in.ras) t = rgba();
  for (Rgba& t : in.kcolor) t = rgba();
  for (Rgba& t : in.reg) t = rgba();
  return in;
}

// ------------------------------------------------------------------ verify
VerifyResult verify(const TevMaterial& m, const PicaProgram& p, uint64_t seed, int n) {
  Rng rng(seed);
  VerifyResult v;
  double sum = 0;
  for (int i = 0; i < n; i++) {
    Inputs in = rng.inputs();
    Rgba a = evalTev(m, in), b = evalPica(p, in), l = evalPica(p, in, false);
    for (int k = 0; k < 4; k++) {
      double e = std::fabs(a.v[k] - b.v[k]) * 255.0;
      v.maxErr255 = std::max(v.maxErr255, e);
      v.logicMaxErr255 = std::max(v.logicMaxErr255, std::fabs(a.v[k] - l.v[k]) * 255.0);
      sum += e;
    }
  }
  v.samples = n;
  v.meanErr255 = n ? sum / (4.0 * n) : 0;
  return v;
}

// ------------------------------------------------------------------ assembly
namespace {

struct Built { PChan ch; std::vector<Demand> dem; std::vector<int> slot; };

bool buildStep(const Step* s, bool alpha, const TevStage& st, Built& b, std::string& why) {
  b = Built{};
  const PArg pass{P_PREV, alpha ? OPR_A : OPR_RGB};
  for (int j = 0; j < 3; j++) b.ch.arg[j] = pass;
  if (!s) return true;  // pass-through: Replace(Previous)
  b.ch.op = s->op; b.ch.scale = s->scale;
  for (int j = 0; j < kArity[s->op]; j++) {
    SrcInfo si;
    if (!leafSource(s->arg[j].leaf, s->arg[j].inv, alpha, st, si, why, s->arg[j].cmp == 1)) return false;
    b.ch.arg[j] = PArg{si.src, si.opr, s->arg[j].cmp != 0};
    if (si.isConst) { b.dem.push_back(si.dem); b.slot.push_back(j); }
  }
  return true;
}

bool makeStage(Built c, Built a, PStage& out, std::string& why) {
  std::vector<Demand> all = c.dem;
  all.insert(all.end(), a.dem.begin(), a.dem.end());
  Alloc al;
  if (!allocate(all, al)) { why = "per-stage constant color cannot hold all constants"; return false; }
  for (size_t i = 0; i < c.slot.size(); i++) c.ch.arg[c.slot[i]].opr = al.opr[i];
  for (size_t i = 0; i < a.slot.size(); i++) a.ch.arg[a.slot[i]].opr = al.opr[c.slot.size() + i];
  out = PStage{};
  out.c = c.ch; out.a = a.ch;
  for (int i = 0; i < 4; i++) out.k[i] = al.k[i];
  return true;
}

// The shorter sequence is placed in the first or the last stages (endAligned); the other stages pass its
// channel through. Which one is hazard-free depends on whether a later step reads the other channel's
// old Previous, so the caller tries both and keeps the one that passes groupOk().
bool assemble(const ChanSeq& sc, const ChanSeq& sa, const TevStage& st, bool endAligned,
              std::vector<PStage>& out, std::string& why) {
  const int nc = int(sc.steps.size()), na = int(sa.steps.size()), n = std::max(nc, na);
  out.clear();
  for (int k = 0; k < n; k++) {
    Built c, a;
    const int oc = endAligned ? n - nc : 0, oa = endAligned ? n - na : 0;
    const Step* cs = (k >= oc && k < oc + nc) ? &sc.steps[k - oc] : nullptr;
    const Step* as = (k >= oa && k < oa + na) ? &sa.steps[k - oa] : nullptr;
    if (!buildStep(cs, false, st, c, why) || !buildStep(as, true, st, a, why)) return false;
    PStage ps;
    if (!makeStage(c, a, ps, why)) return false;
    out.push_back(ps);
  }
  return true;
}

// Local proof for one GC stage: random PREV/regs/textures, ideal-float PICA vs TEV must agree.
// first=true: this GC stage starts the material, so PREV is really 0 and the stage-0 hardware quirk applies.
// bufPrevC / bufPrevA: the stage reads the old PREV's rgb / alpha back through the combiner buffer (translate() saves it there).
bool groupOk(const TevStage& st, const std::vector<PStage>& built, double limit255, bool first, bool bufPrevC = false, bool bufPrevA = false, double* worst = nullptr) {
  PicaProgram p{built};
  Rng rng(0xc0ffee);
  double mx = 0;
  int rc = -1, ra = -1;  // which GC registers the combiner buffer holds for this stage
  for (int r = 0; r < 3; r++) { if (st.regBufC[r]) rc = r; if (st.regBufA[r]) ra = r; }
  for (int i = 0; i < 256; i++) {
    Inputs in = rng.inputs();
    if (!first) in.prev = rng.rgba();
    Rgba lb;  // stage-local proof: P_BUF reads the register's value directly (timing is arranged and checked by translate())
    if (rc >= 0) for (int k = 0; k < 3; k++) lb.v[k] = in.reg[rc].v[k];
    if (ra >= 0) lb.v[3] = in.reg[ra].v[3];
    if (bufPrevC) for (int k = 0; k < 3; k++) lb.v[k] = in.prev.v[k];
    if (bufPrevA) lb.v[3] = in.prev.v[3];
    // compare against the stage's own channel results, not the post-writeback PREV: a channel that GC sends to a
    // REG leaves PREV alone, but the PICA stage still produces it and copies it into the combiner buffer
    Rgba regs[4];
    regs[0] = in.prev;
    for (int k = 0; k < 3; k++) regs[k + 1] = in.reg[k];
    const V3 sc = evalTevChan(st.c, buildLeaves(regs, in, st, false)), sa = evalTevChan(st.a, buildLeaves(regs, in, st, true));
    Rgba a{{sc.v[0], sc.v[1], sc.v[2], sa.v[0]}};
    Rgba b = evalPica(p, in, false, first, &lb);
    for (int k = 0; k < 4; k++) mx = std::max(mx, std::fabs(a.v[k] - b.v[k]) * 255.0);
  }
  if (worst) *worst = mx;
  return mx <= limit255;
}

// ---- temp-register (REG0..2) tracking. The PICA combiner buffer holds ONE rgb and ONE alpha value, written by
// ---- stages 0..3 and readable two stages later. GC code that fits that shape translates exactly.
bool readsColorReg(const TevStage& s, int r) {
  for (Leaf l : s.c.in) if (l == Leaf(L_C0 + 2 * r)) return true;
  return false;
}
bool readsAlphaReg(const TevStage& s, int r) {  // alpha component of REG r, readable from both paths
  for (Leaf l : s.c.in) if (l == Leaf(L_A0 + 2 * r)) return true;
  for (Leaf l : s.a.in) if (l == Leaf(L_A0 + 2 * r)) return true;
  return false;
}
// Is what stage gi wrote into REG r read by a later stage before another stage overwrites it?
bool liveColorReg(const TevMaterial& m, size_t gi, int r) {
  for (size_t k = gi + 1; k < m.stages.size(); k++) {
    if (readsColorReg(m.stages[k], r)) return true;
    if (m.stages[k].c.out == r + 1) return false;
  }
  return false;
}
bool liveAlphaReg(const TevMaterial& m, size_t gi, int r) {
  for (size_t k = gi + 1; k < m.stages.size(); k++) {
    if (readsAlphaReg(m.stages[k], r)) return true;
    if (m.stages[k].a.out == r + 1) return false;
  }
  return false;
}
// A PICA stage always writes its result to Previous. When GC stage gi sends a channel to a REG instead, the GC PREV
// keeps its old value, which PICA cannot do: allowed only if that old value is never read again (the material's
// final output reads PREV too, so a later stage must write it). While PREV has never been written its value is the
// assumed-zero initial one and its reads are folded to ZERO, so they do not count.
bool prevDeadAfter(const TevMaterial& m, size_t gi, bool colorCh, bool prevWritten) {
  for (size_t k = gi + 1; k < m.stages.size(); k++) {
    const TevStage& s = m.stages[k];
    if (prevWritten) {
      if (colorCh) { for (Leaf l : s.c.in) if (l == L_CPREV) return false; }   // (the colour inputs an alpha R8 compare reads are the colour channel's own a and b: covered)
      else {
        for (Leaf l : s.c.in) if (l == L_APREV) return false;
        for (Leaf l : s.a.in) if (l == L_APREV) return false;
      }
    }
    if ((colorCh ? s.c.out : s.a.out) == 0) return true;
  }
  return false;
}
PStage passThroughStage() {
  PStage s;
  for (int j = 0; j < 3; j++) { s.c.arg[j] = PArg{P_PREV, OPR_RGB}; s.a.arg[j] = PArg{P_PREV, OPR_A}; }
  return s;
}

// ---- hoisted compare mask ---------------------------------------------------------------------------------------------------------------------------------------------
// A colour R8 compare costs three PICA stages for its 0/1 mask, all in the colour lane, while the alpha lane of those stages mostly passes a value through. When the compared
// inputs do not depend on Previous (a texture red against HALF, a constant ...), the mask can be computed in the ALPHA lanes of the three stages before the select, in parallel
// with the colour work of the GC stages before the compare, so a material that needs 7 stages sequentially needs 4. Colour and alpha are two independent tracks here:
//   colour track: every GC stage's colour steps in order, the compare contributing only its select (d + mask * c, reading the mask from Previous's alpha lane: L_MIDA)
//   alpha track:  [mask x3] ending one stage before the select, then the alpha steps that are still needed (dead alpha stages are dropped; the first one kept must not read
//                 the old alpha, which the mask overwrote: it is a source-only stage such as KONST * TEXA)
// Only used when the sequential schedule needs more than 6 stages. The result is accepted only after the whole material is proven against the TEV model.
struct TrackEnt { Step s; int stage = -1; };

bool isIdentityStep(const Step& s, bool alpha) {
  return s.op == OP_REPLACE && s.scale == 1 && !s.arg[0].inv && !s.arg[0].cmp && s.arg[0].leaf == (alpha ? L_APREV : L_CPREV);
}

bool translateHoisted(const TevMaterial& m, const Options& opt, Translation& out, std::string& why) {
  const int n = int(m.stages.size());
  int k = -1;
  for (int i = 0; i < n; i++) {
    const TevStage& s = m.stages[i];
    if (s.c.out != 0 || s.a.out != 0) { why = "a stage writes a temp register"; return false; }
    if (s.a.cmp) { why = "alpha channel compare"; return false; }
    for (Leaf l : s.c.in) if (l == L_APREV) { why = "a colour input reads the previous alpha"; return false; }
    if (s.c.cmp) {
      if (k >= 0) { why = "more than one compare"; return false; }
      if (s.c.cmp != 1) { why = "compare kind"; return false; }
      k = i;
    }
  }
  if (k < 0) { why = "no colour compare"; return false; }
  for (int j = 0; j < 2; j++) {
    const Leaf l = m.stages[k].c.in[j];
    if (l == L_CPREV || l == L_APREV) { why = "the compared inputs depend on Previous"; return false; }
  }
  std::vector<TevStage> sts(m.stages.begin(), m.stages.end());
  for (TevChan* ch : {&sts[0].c, &sts[0].a})   // the initial Previous is 0 (see translate())
    for (Leaf& l : ch->in) if (l == L_CPREV || l == L_APREV) l = L_ZERO;

  std::vector<ChanSeq> colSeq(n), alpSeq(n);
  std::vector<Step> mask;
  for (int i = 0; i < n; i++) {
    const ChanResult rc = solveChan(sts[i], false, opt.approxLimit, opt.seed + 1000 * i + 1, i == k);
    const ChanResult ra = solveChan(sts[i], true, opt.approxLimit, opt.seed + 1000 * i + 2);
    if (rc.f != Fidelity::Exact || rc.seqs.empty()) { why = "stage " + std::to_string(i) + " colour: " + rc.note; return false; }
    if (ra.f != Fidelity::Exact || ra.seqs.empty()) { why = "stage " + std::to_string(i) + " alpha: " + ra.note; return false; }
    if (prevBufNeeds(rc.seqs[0], false) || prevBufNeeds(ra.seqs[0], true)) { why = "a stage reads the old PREV through the combiner buffer"; return false; }
    colSeq[i] = rc.seqs[0]; alpSeq[i] = ra.seqs[0];
    if (i == k) mask = rc.mask;
  }

  // colour track
  std::vector<TrackEnt> col;
  int js = -1;
  for (int i = 0; i < n; i++)
    for (const Step& st : colSeq[i].steps) {
      if (isIdentityStep(st, false)) continue;
      if (i == k && js < 0) js = int(col.size());
      col.push_back({st, i});
    }
  if (js < 0) { why = "the compare has no select step"; return false; }
  if (js < 3) { col.insert(col.begin(), size_t(3 - js), TrackEnt{}); js = 3; }   // pad stages so that the mask has three stages before the select

  // alpha track: drop the stages whose result is overwritten before anything reads it
  std::vector<char> keep(size_t(n), 0);
  bool needed = true;
  for (int i = n - 1; i >= 0; i--) {
    keep[size_t(i)] = needed;
    bool readsA = false;
    for (Leaf l : sts[i].a.in) if (l == L_APREV) readsA = true;
    if (keep[size_t(i)]) needed = readsA;
  }
  std::vector<TrackEnt> alp;
  for (int i = 0; i < n; i++) {
    if (!keep[size_t(i)]) continue;
    for (const Step& st : alpSeq[i].steps) {
      if (isIdentityStep(st, true)) continue;
      alp.push_back({st, i});
    }
  }
  if (!alp.empty()) {
    const Step& f = alp[0].s;
    for (int j = 0; j < kArity[f.op]; j++)
      if (f.arg[j].leaf == L_APREV || f.arg[j].leaf == L_MID || f.arg[j].leaf == L_CPREV) { why = "the first kept alpha stage reads the alpha that the mask overwrites"; return false; }
  }

  const int total = std::max(int(col.size()), js + int(alp.size()));
  if (total > 6) { why = "still needs " + std::to_string(total) + " PICA stages"; return false; }

  Translation t;
  for (int p = 0; p < total; p++) {
    const Step* cs = nullptr; const Step* as = nullptr;
    int cst = 0, ast = 0;
    if (p < int(col.size()) && col[size_t(p)].stage >= 0) { cs = &col[size_t(p)].s; cst = col[size_t(p)].stage; }
    if (p >= js - 3 && p < js) { as = &mask[size_t(p - (js - 3))]; ast = k; }
    else if (p >= js && p - js < int(alp.size())) { as = &alp[size_t(p - js)].s; ast = alp[size_t(p - js)].stage; }
    Built c, a;
    PStage ps;
    if (!buildStep(cs, false, sts[size_t(cst)], c, why) || !buildStep(as, true, sts[size_t(ast)], a, why) || !makeStage(c, a, ps, why)) return false;
    t.prog.stages.push_back(ps);
  }
  const VerifyResult vr = verify(m, t.prog, opt.seed ^ 0x51ed270b3c1f9a7dULL, std::max(opt.verifySamples, 512));
  if (vr.logicMaxErr255 > kLogicTol255) {
    why = "the hoisted program does not reproduce the material"; return false;
  }
  t.verify = vr;
  t.overall = Fidelity::Exact;
  for (int i = 0; i < n; i++) {
    StageNote sn;
    sn.gcStage = i;
    sn.color = sn.alpha = Fidelity::Exact;
    int cnt = 0;
    for (const TrackEnt& e : col) if (e.stage == i) cnt++;
    sn.picaStages = cnt;
    if (i == k) sn.text = "color: TEV compare: mask in the alpha lanes of PICA stages " + std::to_string(js - 3) + ".." + std::to_string(js - 1) + ", select in stage " + std::to_string(js) + ". ";
    t.notes.push_back(sn);
  }
  out = t;
  return true;
}

}  // namespace

static Translation translateSearch(const TevMaterial& m, const Options& opt) {
  Translation t;
  Fidelity overall = Fidelity::Exact;
  auto lower = [&](Fidelity f) { if (int(f) < int(overall)) overall = f; };

  bool prevWrittenC = false, prevWrittenA = false;  // has any GC stage written PREV's color / alpha yet?
  int bufCReg = -1, bufCWriter = -1, bufAReg = -1, bufAWriter = -1;  // GC reg held in the combiner buffer, and the PICA stage that wrote it

  for (size_t gi = 0; gi < m.stages.size(); gi++) {
    // PICA stage 0 cannot read a real PREV (it reads 0, or slot 2's source in slots 0/1), so the initial PREV
    // register is assumed to be 0 and its reads are folded to ZERO. ASSUMPTION: the material's initial PREV color
    // is 0 (J3D only ever loads REG0..2: J3DMatBlock.cpp loads mTevColor[0..2] into GX_TEVREG0..2, never TEVPREV).
    TevStage st = m.stages[gi];
    for (TevChan* ch : {&st.c, &st.a})
      for (Leaf& l : ch->in) {
        if (l == L_CPREV && !prevWrittenC) l = L_ZERO;
        if (l == L_APREV && !prevWrittenA) l = L_ZERO;
      }
    for (int r = 0; r < 3; r++) { st.regBufC[r] = bufCReg == r; st.regBufA[r] = bufAReg == r; }
    StageNote note;
    note.gcStage = int(gi);
    ChanResult rc = solveChan(st, false, opt.approxLimit, opt.seed + 1000 * gi + 1);
    ChanResult ra = solveChan(st, true, opt.approxLimit, opt.seed + 1000 * gi + 2);
    note.color = rc.f; note.alpha = ra.f;
    if (rc.f != Fidelity::Exact && !rc.seqs.empty()) note.colorErr255 = rc.seqs[0].err * 255;
    if (ra.f != Fidelity::Exact && !ra.seqs.empty()) note.alphaErr255 = ra.seqs[0].err * 255;
    std::ostringstream txt;
    if (!rc.note.empty()) txt << "color: " << rc.note << ". ";
    if (!ra.note.empty()) txt << "alpha: " << ra.note << ". ";

    if (rc.f == Fidelity::Unsupported || ra.f == Fidelity::Unsupported) {
      lower(Fidelity::Unsupported);
      t.notes.push_back(note);
      t.notes.back().text = txt.str();
      continue;
    }
    // try combinations until the shared per-stage constants fit
    std::vector<PStage> built;
    std::string why;
    bool ok = false;
    const bool exactStage = rc.f == Fidelity::Exact && ra.f == Fidelity::Exact;
    // A sequence that reads the old PREV back through the combiner buffer (search.cpp, L_PBUF) needs the PICA stage before this GC stage
    // to copy its output there: it must be one of stages 0..3, and the buffer must not hold a register that is still needed.
    const int prevIdx = int(t.prog.stages.size()) - 1;
    bool saveC = false, saveA = false;
    std::string saveWhy;
    auto saveOk = [&](bool alphaCh, std::string& w) {
      if (prevIdx < 0 || prevIdx > 3) { w = "the old PREV cannot be saved: only PICA stages 0..3 can write the combiner buffer"; return false; }
      const int r = alphaCh ? bufAReg : bufCReg;
      if (r >= 0 && (alphaCh ? (readsAlphaReg(st, r) || liveAlphaReg(m, gi, r)) : (readsColorReg(st, r) || liveColorReg(m, gi, r)))) {
        w = "the old PREV cannot be saved: the combiner buffer holds a temp register that is still needed";
        return false;
      }
      return true;
    };
    auto attempt = [&](const std::vector<ChanSeq>& cseqs, const std::vector<ChanSeq>& aseqs) {
      for (const ChanSeq& sc : cseqs) {
        for (const ChanSeq& sa : aseqs) {
          const unsigned need = prevBufNeeds(sc, false) | prevBufNeeds(sa, true);
          const bool pbC = (need & PREVBUF_RGB) != 0, pbA = (need & PREVBUF_ALPHA) != 0;
          if ((pbC && !saveOk(false, why)) || (pbA && !saveOk(true, why))) continue;
          for (int al = 0; al < 2 && !ok; al++) {
            if (!assemble(sc, sa, st, al == 1, built, why)) continue;
            // exact stages must reproduce the GC stage exactly (this also catches cross-channel PREV hazards);
            // approximate stages are accepted with their measured error (checked again end to end below)
            if (!exactStage || groupOk(st, built, kLogicTol255, t.prog.stages.empty(), pbC, pbA)) { ok = true; saveC = pbC; saveA = pbA; }
            else why = "channel placement hazard (a later step reads the other channel's old PREV)";
          }
          if (ok) return;
        }
      }
    };
    attempt(rc.seqs, ra.seqs);
    if (!ok && exactStage) {
      // The single-stage solution of a channel can need constant components the other channel's stage needs too (colour lerp(REG1, REG2, TEXC) in two stages next to
      // alpha lerp(A1, A2, TEXA) in one: 3 + 2 components in one stage). The same channel in two stages needs one component per stage: retry with those forms.
      ChanResult rcAlt, raAlt;
      if (!rc.seqs.empty() && rc.seqs[0].steps.size() == 1 && !st.c.cmp) rcAlt = solveChan(st, false, 0.0, opt.seed + 1000 * gi + 3, false, 2);
      if (!ra.seqs.empty() && ra.seqs[0].steps.size() == 1 && !st.a.cmp) raAlt = solveChan(st, true, 0.0, opt.seed + 1000 * gi + 4, false, 2);
      if (!raAlt.seqs.empty()) attempt(rc.seqs, raAlt.seqs);
      if (!ok && !rcAlt.seqs.empty()) attempt(rcAlt.seqs, ra.seqs);
      if (!ok && !rcAlt.seqs.empty() && !raAlt.seqs.empty()) attempt(rcAlt.seqs, raAlt.seqs);
      if (ok) txt << "a channel uses its two-stage form so that the stage constants fit. ";
    }
    if (!ok) {
      lower(Fidelity::Unsupported);
      note.color = note.alpha = Fidelity::Unsupported;
      txt << "assembly failed: " << why;
      note.text = txt.str();
      t.notes.push_back(note);
      continue;
    }
    // ---- a channel that GC sends to a REG while the old PREV is still read later: a PICA stage always overwrites Previous. The buffer's two-stage delay makes room for both: the
    // stage before this GC stage copies the old PREV into the buffer (visible two stages later), this GC stage's last stage copies the REG value into the buffer (visible two stages
    // after it), and one extra stage between the two reads the old PREV back from the buffer into Previous: at that stage the buffer still holds the old PREV.
    bool restoreC = false, restoreA = false;
    std::string restoreWhy;
    if (st.c.out != 0 && !prevDeadAfter(m, gi, true, prevWrittenC) && saveOk(false, restoreWhy)) { restoreC = true; saveC = true; }
    if (st.a.out != 0 && !prevDeadAfter(m, gi, false, prevWrittenA) && saveOk(true, restoreWhy)) { restoreA = true; saveA = true; }
    if (restoreC || restoreA) {
      PStage rs = passThroughStage();
      if (restoreC) rs.c.arg[0] = PArg{P_BUF, OPR_RGB};
      if (restoreA) rs.a.arg[0] = PArg{P_BUF, OPR_A};
      built.push_back(rs);
    }
    const size_t computeLast = built.size() - ((restoreC || restoreA) ? 2 : 1);   // the group's last computing stage, before any pad is inserted below

    // ---- combiner buffer: a P_BUF read must come at least two PICA stages after the stage that wrote the value
    const int base = int(t.prog.stages.size());
    int pad = 0;
    for (size_t j = 0; j < built.size(); j++)
      for (const PChan* ch : {&built[j].c, &built[j].a})
        for (int k = 0; k < kArity[ch->op]; k++) {
          if (ch->arg[k].src != P_BUF) continue;
          const bool alphaBuf = ch->arg[k].opr == OPR_A || ch->arg[k].opr == OPR_1A;
          const int writer = alphaBuf ? (saveA ? base - 1 : bufAWriter) : (saveC ? base - 1 : bufCWriter);   // a saved PREV was written by the stage before this GC stage
          pad = std::max(pad, writer + 2 - (base + int(j)));
        }
    if (pad > 0) built.insert(built.begin(), size_t(pad), passThroughStage());
    const size_t wi = computeLast + size_t(pad);   // index in `built` of the stage whose result is the group's result
    const int lastIdx = base + int(wi);

    // ---- a channel that GC sends to a REG: copy this stage's result into the combiner buffer
    std::string bufWhy;
    int newBufCReg = bufCReg, newBufCWriter = bufCWriter, newBufAReg = bufAReg, newBufAWriter = bufAWriter;
    if (st.c.out != 0) {
      const int r = st.c.out - 1;
      if (!restoreC && !prevDeadAfter(m, gi, true, prevWrittenC)) bufWhy = "color goes to a temp register but the old PREV color is still read later (a PICA stage always overwrites Previous)" + (restoreWhy.empty() ? std::string() : "; it cannot be restored from the buffer: " + restoreWhy);
      else if (liveColorReg(m, gi, r)) {
        if (bufCReg >= 0 && bufCReg != r && liveColorReg(m, gi, bufCReg)) bufWhy = "two temp color registers are live at once (PICA has one combiner buffer)";
        else if (lastIdx > 3) bufWhy = "temp color register needs the combiner buffer, which only PICA stages 0..3 can write";
        else { built[wi].updBufC = true; newBufCReg = r; newBufCWriter = lastIdx; }
      }
    }
    if (bufWhy.empty() && st.a.out != 0) {
      const int r = st.a.out - 1;
      if (!restoreA && !prevDeadAfter(m, gi, false, prevWrittenA)) bufWhy = "alpha goes to a temp register but the old PREV alpha is still read later (a PICA stage always overwrites Previous)" + (restoreWhy.empty() ? std::string() : "; it cannot be restored from the buffer: " + restoreWhy);
      else if (liveAlphaReg(m, gi, r)) {
        if (bufAReg >= 0 && bufAReg != r && liveAlphaReg(m, gi, bufAReg)) bufWhy = "two temp alpha registers are live at once (PICA has one combiner buffer)";
        else if (lastIdx > 3) bufWhy = "temp alpha register needs the combiner buffer, which only PICA stages 0..3 can write";
        else { built[wi].updBufA = true; newBufAReg = r; newBufAWriter = lastIdx; }
      }
    }
    if (!bufWhy.empty()) {
      lower(Fidelity::Unsupported);
      note.color = note.alpha = Fidelity::Unsupported;
      txt << "temp register: " << bufWhy;
      note.text = txt.str();
      t.notes.push_back(note);
      continue;
    }
    bufCReg = newBufCReg; bufCWriter = newBufCWriter; bufAReg = newBufAReg; bufAWriter = newBufAWriter;
    if (saveC) { t.prog.stages[size_t(prevIdx)].updBufC = true; if (st.c.out == 0) { bufCReg = -1; bufCWriter = prevIdx; } }
    if (saveA) { t.prog.stages[size_t(prevIdx)].updBufA = true; if (st.a.out == 0) { bufAReg = -1; bufAWriter = prevIdx; } }
    if (saveC || saveA) txt << "old PREV saved in the combiner buffer by PICA stage " << prevIdx << ". ";
    if (st.c.out == 0) prevWrittenC = true;
    if (st.a.out == 0) prevWrittenA = true;
    if (st.c.out != 0 || st.a.out != 0) txt << "result goes to a temp register via the combiner buffer. ";
    if (restoreC || restoreA) txt << "the old PREV is restored from the combiner buffer after it. ";

    note.picaStages = int(built.size());
    note.text = txt.str();
    for (const PStage& ps : built) t.prog.stages.push_back(ps);
    if (rc.f == Fidelity::Approx || ra.f == Fidelity::Approx) lower(Fidelity::Approx);
    t.notes.push_back(note);
  }

  // The hoisted schedule (see translateHoisted) is the fallback whenever the sequential one does not fit: too many stages, or a stage that could not be assembled (it avoids saving the
  // old PREV in the combiner buffer altogether, which only PICA stages 0..3 can do). It is proven against the TEV model before it is accepted, so it can only add exact materials.
  if (t.prog.stages.size() > 6 || overall == Fidelity::Unsupported) {
    Translation h;
    std::string hwhy;
    if (translateHoisted(m, opt, h, hwhy)) return h;
    if (t.prog.stages.size() > 6) {
      StageNote g;
      g.gcStage = -1;
      g.color = g.alpha = Fidelity::Unsupported;
      g.text = "needs " + std::to_string(t.prog.stages.size()) + " PICA stages, hardware has 6 (needs multipass or stage merging; compare mask in the alpha lanes: " + hwhy + ")";
      t.notes.push_back(g);
      lower(Fidelity::Unsupported);
    } else if (hwhy != "no colour compare") {   // a stage failed for its own reason; say why the hoisted schedule did not rescue the material
      StageNote g;
      g.gcStage = -1;
      g.color = g.alpha = Fidelity::Unsupported;
      g.text = "compare mask in the alpha lanes did not apply: " + hwhy;
      t.notes.push_back(g);
    }
  }

  t.overall = overall;
  if (overall == Fidelity::Unsupported) return t;

  // verification uses a different seed than the search, so overfitting to the search samples shows up
  t.verify = verify(m, t.prog, opt.seed ^ 0x9e3779b97f4a7c15ULL, opt.verifySamples);
  if (overall == Fidelity::Exact && t.verify.logicMaxErr255 > kLogicTol255) {
    StageNote g;
    g.gcStage = -1;
    g.color = g.alpha = Fidelity::Unsupported;
    std::ostringstream o;
    o << "verification FAILED: claimed exact but logic error " << t.verify.logicMaxErr255 << "/255";
    g.text = o.str();
    t.notes.push_back(g);
    t.overall = Fidelity::Unsupported;
  }
  return t;
}

// The search is the expensive part (about 55 ms on a PC per material, see bundle.h): a bundle of results computed offline answers first, a recorder keeps what had to be searched.
Translation translate(const TevMaterial& m, const Options& opt) {
  const MaterialBundle* look = bundleLookup();
  MaterialBundle* rec = bundleRecorder();
  if (!look && !rec) return translateSearch(m, opt);
  const std::string key = translateKey(m, opt);
  if (look) {
    if (const Translation* hit = look->findTranslation(key)) { bundleCountHit(); return *hit; }
  }
  const auto t0 = std::chrono::steady_clock::now();
  Translation t = translateSearch(m, opt);
  if (look) bundleCountMiss(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  if (rec) rec->addTranslation(key, t);
  return t;
}

// ------------------------------------------------------------------ report
static const char* kOpName[OP_COUNT] = {"Replace", "Modulate", "Add", "AddSigned", "Lerp", "Subtract", "MulAdd", "AddMul"};
static const char* kSrcName[] = {"prim", "tex0", "tex1", "tex2", "const", "prev", "buf"};
static const char* kOprName[] = {"", "1-", "a:", "1-a:", "r:", "1-r:", "g:", "1-g:", "b:", "1-b:"};

static std::string chanText(const PChan& c) {
  std::ostringstream o;
  o << kOpName[c.op] << "(";
  for (int j = 0; j < kArity[c.op]; j++) {
    if (j) o << ", ";
    o << kOprName[c.arg[j].opr] << kSrcName[c.arg[j].src];
  }
  o << ")";
  if (c.scale != 1) o << "*" << int(c.scale);
  return o.str();
}

std::string describeStage(const PStage& s) {
  std::ostringstream o;
  o << "C=" << chanText(s.c) << "  A=" << chanText(s.a);
  static const char* cn[4] = {"r", "g", "b", "a"};
  bool any = false;
  for (int i = 0; i < 4; i++) {
    if (!s.k[i].set) continue;
    o << (any ? ", " : "  K{") << cn[i] << "=";
    any = true;
    if (s.k[i].sym == SYM_NONE) o << s.k[i].fixed;
    else {
      o << (s.k[i].inv ? "1-" : "")
        << (s.k[i].sym < SYM_R0 ? "kcolor" + std::to_string(s.k[i].sym - SYM_K0) : "reg" + std::to_string(s.k[i].sym - SYM_R0))
        << "." << cn[s.k[i].chan];
    }
  }
  if (any) o << "}";
  if (s.updBufC || s.updBufA) o << "  ->buffer{" << (s.updBufC ? "rgb" : "") << (s.updBufC && s.updBufA ? "," : "") << (s.updBufA ? "a" : "") << "}";
  return o.str();
}

std::string formatReport(const std::string& name, const TevMaterial& m, const Translation& t) {
  std::ostringstream o;
  o << "== " << name << ": " << fidelityName(t.overall) << " (" << m.stages.size() << " GC stages -> "
    << t.prog.stages.size() << " PICA stages)\n";
  for (const StageNote& n : t.notes) {
    if (n.gcStage < 0) o << "   [global] ";
    else o << "   gc" << n.gcStage << " color=" << fidelityName(n.color) << " alpha=" << fidelityName(n.alpha)
           << " -> " << n.picaStages << " stage(s). ";
    o << n.text << "\n";
  }
  if (t.overall != Fidelity::Unsupported) {
    for (size_t i = 0; i < t.prog.stages.size(); i++) o << "   pica" << i << ": " << describeStage(t.prog.stages[i]) << "\n";
    o << "   verify over " << t.verify.samples << " random inputs: logic error " << t.verify.logicMaxErr255
      << "/255 (ideal float), with PICA 8-bit rounding max " << t.verify.maxErr255 << "/255 mean "
      << t.verify.meanErr255 << "/255\n";
  }
  return o.str();
}

}  // namespace t2p
