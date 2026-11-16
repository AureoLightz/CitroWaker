#include "search.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace t2p {

const int kArity[OP_COUNT] = {1, 2, 2, 2, 3, 2, 3, 3};

unsigned prevBufNeeds(const ChanSeq& q, bool alphaChannel) {
  unsigned m = 0;
  for (const Step& s : q.steps)
    for (int j = 0; j < kArity[s.op]; j++) {
      if (s.arg[j].leaf == L_PBUF) m |= alphaChannel ? PREVBUF_ALPHA : PREVBUF_RGB;
      if (s.arg[j].leaf == L_PBUFA) m |= PREVBUF_ALPHA;
    }
  return m;
}

bool leafSource(Leaf l, bool inv, bool alpha, const TevStage& st, SrcInfo& o, std::string& why, bool red) {
  o = SrcInfo{};
  auto flip = [&](POpr x) { return inv ? POpr(x ^ 1) : x; };
  const bool rgbOk = !alpha || red;   // colour-context leaves are legal in the alpha path only as a red lane
  // operand that reads (swap-mapped) channel(s); rgbLeaf: the leaf is RGB (read whole, or as its red lane), else a single channel
  auto swapOpr = [&](const uint8_t sw[4], bool rgbLeaf, POpr& out) {
    static const POpr base[4] = {OPR_R, OPR_G, OPR_B, OPR_A};
    if (!rgbLeaf) { out = flip(base[sw[3] & 3]); return true; }
    if (red) { out = flip(base[sw[0] & 3]); return true; }
    if (sw[0] == 0 && sw[1] == 1 && sw[2] == 2) { out = flip(OPR_RGB); return true; }
    if (sw[0] == sw[1] && sw[1] == sw[2]) { out = flip(base[sw[0] & 3]); return true; }
    why = "texture/raster swap table mixes channels (no matching PICA operand)";
    return false;
  };
  auto tex = [&](bool rgbLeaf) {
    if (st.texmap < 0 || st.texmap > 2) { why = "texmap outside 0..2 (PICA has 3 texture units)"; return false; }
    o.src = PSrc(P_TEX0 + st.texmap);
    return swapOpr(st.texSwap, rgbLeaf, o.opr);
  };
  auto ras = [&](bool rgbLeaf) {
    if (st.rasChan != 0) { why = "raster channel COLOR1A1 (needs PICA lighting / second color)"; return false; }
    o.src = P_PRIMARY;
    return swapOpr(st.rasSwap, rgbLeaf, o.opr);
  };
  auto constant = [&](Demand d) {
    d.inv = inv; o.src = P_CONST; o.isConst = true; o.dem = d; return true;
  };
  auto bad = [&](const char* w) { why = w; return false; };
  Demand d;
  switch (l) {
    case L_CPREV: if (!rgbOk) return bad("CPREV in alpha path"); o.src = P_PREV; o.opr = flip(red ? OPR_R : OPR_RGB); return true;
    case L_APREV: o.src = P_PREV; o.opr = flip(OPR_A); return true;
    case L_MID: o.src = P_PREV; o.opr = flip(alpha ? OPR_A : OPR_RGB); return true;
    case L_MIDA: if (alpha) return bad("MIDA in alpha path"); o.src = P_PREV; o.opr = flip(OPR_A); return true;
    case L_PBUF: o.src = P_BUF; o.opr = flip(alpha ? OPR_A : OPR_RGB); return true;
    case L_PBUFA: o.src = P_BUF; o.opr = flip(OPR_A); return true;
    case L_TEXC: if (!rgbOk) return bad("TEXC in alpha path"); return tex(true);
    case L_TEXA: return tex(false);
    case L_RASC: if (!rgbOk) return bad("RASC in alpha path"); return ras(true);
    case L_RASA: return ras(false);
    case L_C0: case L_C1: case L_C2:
      if (!rgbOk) return bad("color register in alpha path");
      if (st.regBufC[(l - L_C0) / 2]) { o.src = P_BUF; o.opr = flip(red ? OPR_R : OPR_RGB); return true; }  // value produced by an earlier stage
      d.vec = !red; d.sym = uint8_t(SYM_R0 + (l - L_C0) / 2); return constant(d);
    case L_A0: case L_A1: case L_A2:
      if (st.regBufA[(l - L_A0) / 2]) { o.src = P_BUF; o.opr = flip(OPR_A); return true; }
      d.sym = uint8_t(SYM_R0 + (l - L_A0) / 2); d.chan = 3; return constant(d);
    case L_ONE: d.fixed = true; d.val = 1.0f; return constant(d);
    case L_HALF: d.fixed = true; d.val = 0.5f; return constant(d);
    case L_ZERO: d.fixed = true; d.val = 0.0f; return constant(d);
    case L_KONST: case L_KONSTC: {
      const KSel& k = (l == L_KONSTC || !alpha) ? st.kc : st.ka;
      if (l == L_KONSTC && !red) return bad("KONSTC outside a compare operand");
      if (k.kind == KSel::Fixed) { d.fixed = true; d.val = k.fixed; }
      else if (k.kind == KSel::Rgb) {
        if (alpha && !red) return bad("KCOLOR rgb select in alpha path");
        d.vec = !red; d.sym = uint8_t(SYM_K0 + k.idx);   // red: lane 0 of the colour only (chan 0)
      }
      else { d.sym = uint8_t(SYM_K0 + k.idx); d.chan = k.chan; }
      return constant(d);
    }
    default: return bad("bad leaf");
  }
}

namespace {

struct SolSet {
  std::vector<ChanSeq> sols;
  ChanSeq best;
  bool haveBest = false;
  double bestErr = 1e9;
};

bool demandsOf(const Step& s, bool alpha, const TevStage& st, std::vector<Demand>& dm) {
  dm.clear();
  std::string w;
  for (int j = 0; j < kArity[s.op]; j++) {
    SrcInfo si;
    if (!leafSource(s.arg[j].leaf, s.arg[j].inv, alpha, st, si, w, s.arg[j].cmp == 1)) return false;
    if (si.isConst) dm.push_back(si.dem);
  }
  return true;
}

bool feasible(const Step& s, bool alpha, const TevStage& st) {
  std::vector<Demand> dm;
  Alloc al;
  return demandsOf(s, alpha, st, dm) && allocate(dm, al);
}

double stepErr(const Step& s, bool alpha, const std::vector<LeafTable>& smp, const std::vector<V3>& exp,
               double cutoff) {
  const int lanes = alpha ? 1 : 3, ar = kArity[s.op];
  double mx = 0;
  for (size_t i = 0; i < smp.size(); i++) {
    V3 a[3];
    for (int j = 0; j < ar; j++) {
      a[j] = smp[i][s.arg[j].leaf];
      if (s.arg[j].cmp == 1) a[j].v[1] = a[j].v[2] = a[j].v[0];   // the red lane, broadcast
      if (s.arg[j].inv) for (int k = 0; k < 3; k++) a[j].v[k] = 1 - a[j].v[k];
      if (s.arg[j].cmp) for (int k = 0; k < 3; k++) a[j].v[k] = snap8(a[j].v[k]);
    }
    V3 r = picaChanEval(s.op, a, s.scale, false);
    for (int k = 0; k < lanes; k++) mx = std::max(mx, double(std::fabs(r.v[k] - exp[i].v[k])));
    if (mx > cutoff) return mx;
  }
  return mx;
}

void search(bool alpha, const TevStage& st, const std::vector<LeafTable>& smp, const std::vector<V3>& exp,
            const std::vector<Leaf>& pool, SolSet& out, size_t maxSols, std::string& why) {
  std::vector<ArgSel> cand;
  for (Leaf l : pool)
    for (int inv = 0; inv < 2; inv++) {
      SrcInfo si; std::string w;
      if (leafSource(l, inv == 1, alpha, st, si, w)) cand.push_back({l, inv == 1});
      else if (why.empty()) why = w;
    }
  static const uint8_t scales[3] = {1, 2, 4};
  const size_t n = cand.size();
  if (!n) return;
  for (int op = 0; op < OP_COUNT && out.sols.size() < maxSols; op++) {
    const int ar = kArity[op];
    size_t total = 1;
    for (int j = 0; j < ar; j++) total *= n;
    for (uint8_t sc : scales) {
      for (size_t idx = 0; idx < total && out.sols.size() < maxSols; idx++) {
        Step s; s.op = POp(op); s.scale = sc;
        size_t t = idx;
        for (int j = 0; j < ar; j++) { s.arg[j] = cand[t % n]; t /= n; }
        double cutoff = out.sols.empty() ? std::max(out.bestErr, 2e-4) : 2e-4;
        double e = stepErr(s, alpha, smp, exp, cutoff);
        if (e > cutoff) continue;
        if (e <= 2e-4) {
          if (feasible(s, alpha, st)) { ChanSeq q; q.steps.push_back(s); out.sols.push_back(q); }
        } else if (e < out.bestErr && feasible(s, alpha, st)) {
          out.best.steps.assign(1, s); out.best.err = e; out.bestErr = e; out.haveBest = true;
        }
      }
    }
  }
}

int demandCount(const ChanSeq& q, bool alpha, const TevStage& st) {
  int c = 0;
  std::vector<Demand> dm;
  for (const Step& s : q.steps) { demandsOf(s, alpha, st, dm); c += int(dm.size()); }
  return c;
}

// In a second PICA stage the old PREV is gone from Previous (the first stage overwrote it): a GC input that reads it is read back from the combiner buffer.
Leaf prevAlias(Leaf l, bool alpha) {
  if (l == L_CPREV) return L_PBUF;
  if (l == L_APREV) return alpha ? L_PBUF : L_PBUFA;
  return l;
}
void fillPrevAliases(std::vector<LeafTable>& smp, bool alpha) {
  for (LeafTable& t : smp) { t[L_PBUF] = alpha ? t[L_APREV] : t[L_CPREV]; t[L_PBUFA] = t[L_APREV]; }
}

void addPool(std::vector<Leaf>& p, Leaf l) {
  if (std::find(p.begin(), p.end(), l) == p.end()) p.push_back(l);
}

// Solves one channel from sample tables: the expected value of every sample is the channel's own TEV result. The sample tables carry whatever extra leaves the channel needs
// (L_MID, L_PBUF, L_PBUFA). A single PICA stage first; then two stages (lerp + finish, partial result first, constant first); then the best single-stage fit as an approximation.
ChanResult solveSamples(const TevChan& ch, bool alpha, const TevStage& st, const std::vector<LeafTable>& smp, double approxLimit, int depth = 0, int minSteps = 1) {
  ChanResult res;
  std::string why;
  std::vector<V3> exp;
  for (const LeafTable& lt : smp) exp.push_back(evalTevChan(ch, lt));

  auto sortTrim = [&](SolSet& s, size_t keep) {
    std::stable_sort(s.sols.begin(), s.sols.end(), [&](const ChanSeq& x, const ChanSeq& y) {
      return demandCount(x, alpha, st) < demandCount(y, alpha, st);
    });
    if (s.sols.size() > keep) s.sols.resize(keep);
  };

  std::vector<Leaf> pool;
  for (int j = 0; j < 4; j++) addPool(pool, ch.in[j]);
  addPool(pool, L_ONE); addPool(pool, L_HALF); addPool(pool, L_ZERO);

  // depth 1: stage 0 of the chain already loaded one constant leaf into Previous (smp[L_MID] holds its value); this call solves the rest in two stages. A single stage
  // that does not need L_MID would have been found by the caller.
  SolSet s1;
  if (depth == 0 && minSteps < 2) {
    search(alpha, st, smp, exp, pool, s1, 12, why);
    if (!s1.sols.empty()) { sortTrim(s1, 8); res.seqs = s1.sols; res.f = Fidelity::Exact; return res; }
  }

  // two PICA stages: A = lerp(a,b,c) -> Previous, B = (d +- Previous + bias) * scale.
  // Stage A overwrites Previous. Whatever stage B reads of the old PREV (here: d) comes back through the combiner buffer (L_PBUF / L_PBUFA): translate() makes the
  // stage before A copy its output there (a buffer read comes two stages after the write, which is exactly stage B). Possible only when that stage is one of PICA
  // stages 0..3 and the buffer is free.
  {
    TevChan ca;
    ca.in[0] = ch.in[0]; ca.in[1] = ch.in[1]; ca.in[2] = ch.in[2]; ca.in[3] = L_ZERO;
    std::vector<V3> expA;
    for (auto& lt : smp) expA.push_back(evalTevChan(ca, lt));
    std::vector<Leaf> poolA;
    for (int j = 0; j < 3; j++) addPool(poolA, ch.in[j]);
    addPool(poolA, L_ONE); addPool(poolA, L_HALF); addPool(poolA, L_ZERO);
    if (depth == 1) addPool(poolA, L_MID);
    SolSet sa;
    search(alpha, st, smp, expA, poolA, sa, 6, why);
    if (!sa.sols.empty()) {
      sortTrim(sa, 4);
      const Leaf dLeaf = prevAlias(ch.in[3], alpha);
      TevChan cb;
      cb.in[0] = L_ZERO; cb.in[1] = L_MID; cb.in[2] = L_ONE; cb.in[3] = dLeaf;
      cb.sub = ch.sub; cb.bias = ch.bias; cb.scale = ch.scale; cb.clamp = ch.clamp;
      std::vector<LeafTable> smpB = smp;
      fillPrevAliases(smpB, alpha);
      std::vector<V3> expB;
      for (size_t i = 0; i < smpB.size(); i++) { smpB[i][L_MID] = expA[i]; expB.push_back(evalTevChan(cb, smpB[i])); }
      std::vector<Leaf> poolB;
      addPool(poolB, L_MID); addPool(poolB, dLeaf);
      addPool(poolB, L_ONE); addPool(poolB, L_HALF); addPool(poolB, L_ZERO);
      SolSet sb;
      search(alpha, st, smpB, expB, poolB, sb, 6, why);
      if (!sb.sols.empty()) {
        sortTrim(sb, 4);
        for (auto& x : sa.sols)
          for (auto& y : sb.sols) {
            ChanSeq q; q.steps = {x.steps[0], y.steps[0]};
            res.seqs.push_back(q);
          }
        res.f = Fidelity::Exact;
        res.note = dLeaf != ch.in[3] ? "split in two PICA stages (old PREV read back from the combiner buffer)" : "split in two PICA stages";
        if (minSteps < 2) return res;   // minSteps >= 2: also collect the other forms (their constants are placed differently)
      }
    }
  }

  // two PICA stages, general form: stage A = one step over the equation's leaves (a product, a sum, a difference or a copy of one or two of them), stage B finishes
  // the equation from that partial result. Reaches what "lerp, then add d" cannot: lerp(C1, C2, x) = C2*x + C1*(1-x) has two different constants (one per stage on
  // the PICA), so A = x * C2 and B = (1-x) * C1 + A. Leaves of the old PREV that B needs come back through the combiner buffer.
  {
    std::vector<Leaf> leaves;
    for (int j = 0; j < 4; j++) if (ch.in[j] != L_ZERO) addPool(leaves, ch.in[j]);
    std::vector<Leaf> leavesA = leaves;
    if (depth == 1) addPool(leavesA, L_MID);
    std::vector<ArgSel> av;
    for (Leaf l : leavesA)
      for (int inv = 0; inv < 2; inv++) {
        SrcInfo si; std::string w;
        if (leafSource(l, inv == 1, alpha, st, si, w)) av.push_back({l, inv == 1});
      }
    std::vector<Step> as;
    for (size_t i = 0; i < av.size(); i++) {
      Step r; r.op = OP_REPLACE; r.arg[0] = av[i]; as.push_back(r);
      for (size_t j = 0; j < av.size(); j++) {
        if (av[i].leaf == av[j].leaf && av[i].inv == av[j].inv) continue;
        Step m; m.op = OP_MODULATE; m.arg[0] = av[i]; m.arg[1] = av[j];
        if (i < j) as.push_back(m);                       // commutative
        Step a; a.op = OP_ADD; a.arg[0] = av[i]; a.arg[1] = av[j];
        if (i < j) as.push_back(a);
        Step d; d.op = OP_SUB; d.arg[0] = av[i]; d.arg[1] = av[j];
        as.push_back(d);
      }
    }
    for (const Step& A : as) {
      if (!feasible(A, alpha, st)) continue;
      std::vector<LeafTable> smpB = smp;
      fillPrevAliases(smpB, alpha);
      bool trivial = true;   // A's value must not be constant over the samples (it would not carry information)
      for (size_t i = 0; i < smpB.size(); i++) {
        V3 a[3];
        for (int j = 0; j < kArity[A.op]; j++) {
          a[j] = smp[i][A.arg[j].leaf];
          if (A.arg[j].inv) for (float& f : a[j].v) f = 1 - f;
        }
        smpB[i][L_MID] = picaChanEval(A.op, a, 1, false);
        if (i && std::fabs(smpB[i][L_MID].v[0] - smpB[0][L_MID].v[0]) > 1e-6f) trivial = false;
      }
      if (trivial) continue;
      std::vector<Leaf> poolB;
      addPool(poolB, L_MID);
      for (Leaf l : leaves) addPool(poolB, prevAlias(l, alpha));
      addPool(poolB, L_ONE); addPool(poolB, L_HALF); addPool(poolB, L_ZERO);
      SolSet sb;
      std::string w2;
      search(alpha, st, smpB, exp, poolB, sb, 4, w2);
      if (sb.sols.empty()) continue;
      sortTrim(sb, 4);
      for (auto& y : sb.sols) { ChanSeq q; q.steps = {A, y.steps[0]}; res.seqs.push_back(q); }
      res.f = Fidelity::Exact;
      res.note = "split in two PICA stages (partial result first)";
      if (minSteps < 2) return res;
      if (res.seqs.size() >= 12) break;
    }
  }

  // two PICA stages, constant first: stage A = Replace(x) puts one constant leaf into Previous, stage B does
  // all the TEV math with x read from Previous. One constant per stage means two different constants fit.
  if (depth == 0) {
    bool readsPrev = false;
    for (int j = 0; j < 4; j++) readsPrev |= ch.in[j] == L_CPREV || ch.in[j] == L_APREV || ch.in[j] == L_MID || ch.in[j] == L_MIDA;   // L_MID / L_MIDA: a compare's mask also sits in Previous
    if (!readsPrev) {
      static const Leaf cand[] = {L_C0, L_A0, L_C1, L_A1, L_C2, L_A2, L_KONST, L_ONE, L_HALF};
      for (Leaf x : cand) {
        bool used = false;
        for (int j = 0; j < 4; j++) used |= ch.in[j] == x;
        if (!used) continue;
        for (int inv = 0; inv < 2; inv++) {
          SrcInfo si; std::string w;
          if (!leafSource(x, inv == 1, alpha, st, si, w)) continue;
          std::vector<LeafTable> smpB = smp;
          for (auto& lt : smpB) {
            V3 v = lt[x];
            if (inv) for (float& f : v.v) f = 1 - f;
            lt[L_MID] = v;
          }
          std::vector<Leaf> poolB;
          for (int j = 0; j < 4; j++) addPool(poolB, ch.in[j]);
          addPool(poolB, L_MID); addPool(poolB, L_ONE); addPool(poolB, L_HALF); addPool(poolB, L_ZERO);
          SolSet sb;
          std::string w2;
          search(alpha, st, smpB, exp, poolB, sb, 4, w2);
          if (sb.sols.empty()) continue;
          sortTrim(sb, 4);
          Step a; a.op = OP_REPLACE; a.arg[0] = {x, inv == 1}; a.scale = 1;
          for (auto& y : sb.sols) { ChanSeq q; q.steps = {a, y.steps[0]}; res.seqs.push_back(q); }
          res.f = Fidelity::Exact;
          res.note = "split in two PICA stages (constant loaded first)";
          if (minSteps < 2) return res;
          if (res.seqs.size() >= 12) break;
        }
      }
    }
  }

  if (!res.seqs.empty()) return res;   // minSteps >= 2: the two-stage forms found above

  // three PICA stages, constant first: stage A = Replace(x) puts one constant leaf into Previous, then the equation is solved in two more stages in which stage B may read x
  // from Previous (x stays an ordinary constant leaf in the stage after that). This is what an equation with three different constants needs:
  // lerp(REG0, KONST, TEXC) + REG1 is A = REG0, B = lerp(KONST, Previous, TEXC), C = Previous + REG1. Exact for every value of the constants because the program does not
  // depend on them. The old PREV (which stage A overwrites) is read back through the combiner buffer, like in the other splits; not when the equation uses a compare mask.
  if (depth == 0) {
    bool readsMid = false;
    for (int j = 0; j < 4; j++) readsMid |= ch.in[j] == L_MID || ch.in[j] == L_MIDA;
    if (!readsMid) {
      static const Leaf cand[] = {L_C0, L_A0, L_C1, L_A1, L_C2, L_A2, L_KONST, L_ONE, L_HALF};
      TevChan rest = ch;
      for (Leaf& l : rest.in) l = prevAlias(l, alpha);
      for (Leaf x : cand) {
        bool used = false;
        for (int j = 0; j < 4; j++) used |= ch.in[j] == x;
        if (!used) continue;
        SrcInfo si; std::string w;
        if (!leafSource(x, false, alpha, st, si, w)) continue;
        std::vector<LeafTable> smpB = smp;
        fillPrevAliases(smpB, alpha);
        for (auto& lt : smpB) lt[L_MID] = lt[x];
        ChanResult sub = solveSamples(rest, alpha, st, smpB, 0.0, 1);
        if (sub.f != Fidelity::Exact || sub.seqs.empty()) continue;
        Step a; a.op = OP_REPLACE; a.arg[0] = {x, false}; a.scale = 1;
        for (const ChanSeq& y : sub.seqs) {
          ChanSeq q; q.steps.push_back(a); q.steps.insert(q.steps.end(), y.steps.begin(), y.steps.end());
          res.seqs.push_back(q);
          if (res.seqs.size() >= 4) break;
        }
        res.f = Fidelity::Exact;
        res.note = "split in three PICA stages (constant loaded first)";
        return res;
      }
    }
  }

  if (s1.haveBest && s1.bestErr <= approxLimit) {
    res.seqs.push_back(s1.best);
    res.f = Fidelity::Approx;
    std::ostringstream o;
    o << "best single-stage fit, max error " << s1.bestErr * 255 << "/255";
    res.note = o.str();
    return res;
  }
  std::ostringstream o;
  o << "no exact mapping in <=2 stages";
  if (s1.haveBest) o << "; best approximation error " << s1.bestErr * 255 << "/255 exceeds limit";
  if (res.note.size()) o << "; " << res.note;
  res.note = o.str();
  return res;
}

}  // namespace

ChanResult solveChan(const TevStage& st, bool alpha, double approxLimit, uint64_t seed, bool hoistMask, int minSteps) {
  ChanResult res;
  const TevChan& ch = alpha ? st.a : st.c;
  std::string why;

  // every leaf the GC stage reads must be reachable on PICA, otherwise "approximating" would just drop it
  for (int j = 0; j < 4; j++) {
    SrcInfo si;
    if (!leafSource(ch.in[j], false, alpha, st, si, why, ch.cmp == 1 && j < 2)) { res.note = why; return res; }
  }
  if (ch.cmp == 2 && !alpha) { res.note = "A8 compare in the colour channel"; return res; }
  if (ch.cmp > 2) { res.note = "unknown compare kind"; return res; }
  // ch.out != 0 (result goes to a temp register) is handled by translate(): the result is also copied into the combiner buffer

  Rng rng(seed);
  std::vector<LeafTable> smp;
  for (int i = 0; i < 64; i++) {
    Inputs in = rng.inputs();
    Rgba regs[4];
    regs[0] = rng.rgba();
    for (int k = 0; k < 3; k++) regs[k + 1] = in.reg[k];
    smp.push_back(buildLeaves(regs, in, st, alpha));
  }

  // TEV compare: out = d + (a > b ? c : 0). Three PICA stages make a 0/1 mask, the rest is an ordinary TEV equation over the mask:
  //   1  MID = clamp(4 * (a - b))     the compared operands are 8 bit levels, so a > b gives at least 4/255 and anything else gives 0
  //   2  MID = clamp(4 * (MID + MID)) x8 per stage: 4/255 -> 32/255 -> 256/255, so MID is exactly 1 for a > b and exactly 0 otherwise
  //   3  MID = clamp(4 * (MID + MID))
  //   then d + mask * c = d + lerp(0, c, mask): solved like any channel (a single stage, e.g. MulAdd(MID, c, d), or two when c and d need different constants). Stage 1
  //   destroyed the old PREV, so inputs c / d that read it come back through the combiner buffer (prevAlias).
  // R8 compares read the red lane of a and b (the swap tables of TEXC / RASC may mix channels: only the red lane's source matters)
  if (hoistMask && !(ch.cmp == 1 && !alpha)) { res.note = "hoisted mask: only the colour channel's R8 compare"; return res; }
  if (ch.cmp) {
    const uint8_t cmpMode = ch.cmp == 1 ? 1 : 2;
    Step s1; s1.op = OP_SUB; s1.scale = 4;
    s1.arg[0] = ArgSel{ch.in[0], false, cmpMode}; s1.arg[1] = ArgSel{ch.in[1], false, cmpMode};
    Step s2; s2.op = OP_ADD; s2.scale = 4;
    s2.arg[0] = ArgSel{L_MID, false, 0}; s2.arg[1] = ArgSel{L_MID, false, 0};
    if (hoistMask) {
      // the mask runs in the alpha lanes: the same three steps as alpha-path steps (the compared inputs are read as red lanes, which the alpha path allows). In the alpha
      // path L_KONST means the ALPHA constant selection, so a compared KONST is the colour selection L_KONSTC there.
      for (int j = 0; j < 2; j++) if (s1.arg[j].leaf == L_KONST) s1.arg[j].leaf = L_KONSTC;
      if (!feasible(s1, true, st)) { res.note = "compare: the two compared inputs cannot be read by the alpha path of one stage"; return res; }
      TevChan v;
      v.in[0] = L_ZERO; v.in[1] = ch.in[2]; v.in[2] = L_MIDA; v.in[3] = ch.in[3];
      v.clamp = ch.clamp;
      std::vector<LeafTable> smpB = smp;
      for (size_t i = 0; i < smpB.size(); i++) {
        const float m = tevCompareGT(ch, smp[i]) ? 1.0f : 0.0f;
        smpB[i][L_MIDA] = V3{{m, m, m}};
        const V3 want = evalTevChan(ch, smp[i]), got = evalTevChan(v, smpB[i]);
        for (int k = 0; k < 3; k++) if (std::fabs(want.v[k] - got.v[k]) > 1e-6f) { res.note = "compare: internal error, the mask equation differs from the compare"; return res; }
      }
      ChanResult fin = solveSamples(v, false, st, smpB, 0.0);
      if (fin.f != Fidelity::Exact || fin.seqs.empty()) { res.note = "compare: no exact PICA mapping of d + (a > b ? c : 0) from the mask" + (fin.note.empty() ? std::string() : "; " + fin.note); return res; }
      for (const ChanSeq& f : fin.seqs) res.seqs.push_back(f);
      res.mask = {s1, s2, s2};
      res.f = Fidelity::Exact;
      res.note = "TEV compare: mask in the alpha lanes of three earlier stages, then " + std::to_string(fin.seqs[0].steps.size()) + " to select";
      return res;
    }
    if (!feasible(s1, alpha, st)) { res.note = "compare: the two compared inputs cannot share the stage constant"; return res; }
    TevChan v;
    v.in[0] = L_ZERO; v.in[1] = prevAlias(ch.in[2], alpha); v.in[2] = L_MID; v.in[3] = prevAlias(ch.in[3], alpha);
    v.clamp = ch.clamp;
    std::vector<LeafTable> smpB = smp;
    fillPrevAliases(smpB, alpha);
    for (size_t i = 0; i < smpB.size(); i++) {
      const float m = tevCompareGT(ch, smp[i]) ? 1.0f : 0.0f;
      smpB[i][L_MID] = V3{{m, m, m}};
      const V3 want = evalTevChan(ch, smp[i]), got = evalTevChan(v, smpB[i]);
      for (int k = 0; k < 3; k++) if (std::fabs(want.v[k] - got.v[k]) > 1e-6f) { res.note = "compare: internal error, the mask equation differs from the compare"; return res; }
    }
    ChanResult fin = solveSamples(v, alpha, st, smpB, 0.0);
    if (fin.f != Fidelity::Exact || fin.seqs.empty()) { res.note = "compare: no exact PICA mapping of d + (a > b ? c : 0) from the mask" + (fin.note.empty() ? std::string() : "; " + fin.note); return res; }
    for (const ChanSeq& f : fin.seqs) {
      ChanSeq q;
      q.steps = {s1, s2, s2};
      q.steps.insert(q.steps.end(), f.steps.begin(), f.steps.end());
      res.seqs.push_back(q);
    }
    res.f = Fidelity::Exact;
    res.note = "TEV compare: mask in three PICA stages (subtract, saturate twice), then " + std::to_string(fin.seqs[0].steps.size()) + " to select";
    return res;
  }

  return solveSamples(ch, alpha, st, smp, approxLimit, 0, minSteps);
}

}  // namespace t2p
