// A small PICA200 vertex-shader assembler front end + interpreter for the subset of picasso syntax that tev2pica emits.
// Purpose: check generated shaders on the PC (logic, swizzles, register use) and enforce picasso's operand rules, so a
// shader that reaches the console build has already run once. It is NOT picasso: instruction encodings are not produced.
// The rules enforced here were read from picasso's own source (picasso_assembler.cpp, dkp/picasso 2.x):
//   * 2-source ops: src1 wide (v/r/c, relative addressing), src2 narrow (v/r). dph/sge/slt also accept narrow,wide.
//   * mad: src1 narrow; of src2/src3 exactly one may be wide. 1-source ops: src wide.
//   * only one distinct input register (v) may be named per instruction; outputs (o) are write-only, and picasso
//     would silently read input register N for source "oN", so this front end rejects it.
//   * at most 512 instructions, 128 operand descriptors (32 for mad), uniforms c0..c95 shared by .fvec (from c0 up)
//     and .constf (from c95 down), outputs o0..o6, two consecutive mova freeze the GPU.
// Arithmetic is IEEE float32 by default (Azahar's behaviour); runPica(..., f24=true) rounds every result to float24's
// 16-bit mantissa to estimate the real chip.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace t2p {

struct PVec { float v[4] = {0, 0, 0, 0}; };

struct PicaOperand {
  char file = 'r';   // 'v' input, 'r' temp, 'c' uniform/constant, 'o' output, 'a' address register (mova dest)
  int reg = 0;       // register number within its file (alias and constant offset already folded in)
  int idx = 0;       // relative addressing: 0 none, 1 a0.x, 2 a0.y, 3 aL
  bool neg = false;
  int swz[4] = {0, 1, 2, 3};
};
struct PicaIns {
  std::string op;
  PicaOperand dst, src[3];
  bool dstMask[4] = {true, true, true, true};
  int nsrc = 0;
  int line = 0;
};
struct PicaOutDecl { std::string prop; int reg = 0; int mask = 0xF; };  // mask: bit0 = x
struct PicaProgram2 {
  std::vector<PicaIns> ins;
  std::map<std::string, std::pair<int, int>> uniform;   // .fvec name -> (first register, size)
  std::map<int, PVec> constants;                        // register -> value (.constf)
  std::map<std::string, int> inputs;                    // .in name -> v register
  std::vector<PicaOutDecl> outputs;
  int fvecRegs = 0;                                     // .fvec registers allocated (c0..)
  int constRegs = 0;                                    // .constf registers allocated (from c95 down)
  int opdescs = 0, madOpdescs = 0;                      // operand descriptors picasso would need (limit 128 / 32)
};

// Parses and validates. On failure returns false and err describes the first problem (with line number).
bool parsePica(const std::string& text, PicaProgram2& out, std::string& err);

// Runs the program. uniforms: .fvec name -> array of vec4 (shorter than declared is fine, rest zero). in[i] = vN.
// Outputs are keyed by property: "position", "color", "texcoord0", "texcoord0w" (lane 0), "texcoord1", "texcoord2";
// components not written stay 0. Returns false with err on a runtime fault (bad relative address, unknown uniform).
bool runPica(const PicaProgram2&, const std::map<std::string, std::vector<PVec>>& uniforms, const PVec in[16],
             std::map<std::string, PVec>& out, std::string& err, bool f24 = false);

float roundF24(float);

}  // namespace t2p
