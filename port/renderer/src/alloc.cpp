#include "alloc.h"

#include <cmath>

namespace t2p {
namespace {

bool compat(const KComp& have, const KComp& want) {
  if (!have.set) return true;
  if (have.sym != want.sym) return false;
  if (have.sym == SYM_NONE) return std::fabs(have.fixed - want.fixed) < 1e-6f;
  return have.chan == want.chan && have.inv == want.inv;
}

bool rec(const std::vector<Demand>& d, size_t i, KComp k[4], std::vector<POpr>& opr) {
  if (i == d.size()) return true;
  const Demand& dm = d[i];
  for (int si = 0; si < 2; si++) {  // si: store the inverted value in K
    bool oprInv = dm.inv ^ (si == 1);
    if (dm.vec) {
      KComp save[4];
      for (int c = 0; c < 4; c++) save[c] = k[c];
      bool ok = true;
      for (int c = 0; c < 3 && ok; c++) {
        KComp w; w.set = true; w.sym = dm.sym; w.chan = uint8_t(c); w.inv = si == 1;
        if (!compat(k[c], w)) ok = false; else k[c] = w;
      }
      if (ok) {
        opr[i] = oprInv ? OPR_1RGB : OPR_RGB;
        if (rec(d, i + 1, k, opr)) return true;
      }
      for (int c = 0; c < 4; c++) k[c] = save[c];
    } else {
      for (int x = 0; x < 4; x++) {
        KComp w; w.set = true;
        if (dm.fixed) { w.sym = SYM_NONE; w.fixed = si ? 1 - dm.val : dm.val; }
        else { w.sym = dm.sym; w.chan = dm.chan; w.inv = si == 1; }
        if (!compat(k[x], w)) continue;
        KComp save = k[x];
        k[x] = w;
        static const POpr base[4] = {OPR_R, OPR_G, OPR_B, OPR_A};
        opr[i] = POpr(base[x] ^ (oprInv ? 1 : 0));
        if (rec(d, i + 1, k, opr)) return true;
        k[x] = save;
      }
    }
  }
  return false;
}

}  // namespace

bool allocate(const std::vector<Demand>& d, Alloc& out) {
  out = Alloc{};
  out.opr.assign(d.size(), OPR_RGB);
  return rec(d, 0, out.k, out.opr);
}

}  // namespace t2p
