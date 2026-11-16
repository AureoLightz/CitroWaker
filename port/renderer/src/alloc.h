// Per-stage PICA constant-color allocator.
// A stage has ONE RGBA constant shared by every Constant-sourced argument of both
// the color and alpha path. Each demand (a GC constant leaf) must be expressible as
// a component choice + operand (rgb / broadcast r,g,b,a / one-minus of those).
#pragma once
#include "model.h"

namespace t2p {

struct Demand {
  bool vec = false;     // true: needs the whole symbol rgb vector
  bool fixed = false;   // uniform fixed value `val`
  float val = 0;        // base value when fixed
  uint8_t sym = SYM_NONE, chan = 0;  // symbol (+channel for uniform)
  bool inv = false;     // wanted value is 1 - base
};

struct Alloc {
  KComp k[4];
  std::vector<POpr> opr;  // operand chosen for each demand, same order as the input
};

bool allocate(const std::vector<Demand>&, Alloc&);

}  // namespace t2p
