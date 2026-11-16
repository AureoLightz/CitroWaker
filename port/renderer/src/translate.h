#pragma once
#include <string>
#include <vector>

#include "model.h"

namespace t2p {

enum class Fidelity { Exact = 2, Approx = 1, Unsupported = 0 };
const char* fidelityName(Fidelity);

struct StageNote {
  int gcStage = 0;
  Fidelity color = Fidelity::Exact, alpha = Fidelity::Exact;
  int picaStages = 0;
  double colorErr255 = 0, alphaErr255 = 0;  // search-time error, only meaningful for Approx
  std::string text;
};

struct VerifyResult {
  double logicMaxErr255 = 0;             // ideal-float PICA vs TEV: must be ~0 for anything claimed Exact
  double maxErr255 = 0, meanErr255 = 0;  // with PICA 8-bit rounding; informational (gain amplifies it)
  int samples = 0;
};

class FogCompileCache;
struct Options {
  FogCompileCache* fogCompileCache = nullptr; // host cache only; no shader/material semantic change
  bool packedUniforms = false; // host preparation only; identical shader/material semantics
  double approxLimit = 0.20;  // refuse approximations worse than this (fraction of full scale)
  int verifySamples = 4000;
  uint64_t seed = 0x5eed1234abcdULL;
  float frameFogRange = 0.0f; // >0: the frame uses fog with this W-buffer range D (frame policy, see frameFogRange() in draw.h): every draw gets the W map, fogged ones a LUT built for D
  bool flipTexT = true;       // vertex shader outputs t' = q - t (PICA texture rows run bottom-up: hwcheck4); false only for experiments
  bool multipass = true;      // compileDraw may split a draw that does not fit one pass into two (draw.h, multipass.cpp)
  int approxEffort = 10;      // approximate rung: compile attempts per degradation round (approx.h); 0 = only the last resort (texture x vertex colour), which costs one compile
  bool approximate = true;    // compileDraw may degrade a material that cannot be drawn exactly (approx.h): the draw is APPROX with the steps in its notes, instead of refused
};

struct Translation {
  Fidelity overall = Fidelity::Unsupported;
  PicaProgram prog;                // valid only when overall != Unsupported
  std::vector<StageNote> notes;    // one per GC stage, plus global notes (gcStage = -1)
  VerifyResult verify;
};

Translation translate(const TevMaterial&, const Options& = {});
VerifyResult verify(const TevMaterial&, const PicaProgram&, uint64_t seed, int n);
constexpr double kLogicTol255 = 0.05;  // float-noise allowance for the logic check, in 1/255 units
std::string describeStage(const PStage&);
std::string formatReport(const std::string& name, const TevMaterial&, const Translation&);

// deterministic RNG shared with the tests
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
  uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 2685821657736338717ULL; }
  float f() { return float(next() >> 40) / 16777216.0f; }
  float v8();                      // 8-bit value, with extra weight on 0 and 1
  Rgba rgba();
  Inputs inputs();
};

}  // namespace t2p
