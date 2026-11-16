// PICA200 vertex-shader assembler: .pica text -> SHBIN (the container libctru's DVLB_ParseFile / shaderProgramSetVsh read).
//
// A port of devkitPro's picasso (picasso_assembler.cpp / picasso_frontend.cpp, dkp/picasso 2.x) restricted to what a vertex shader needs, so the runtime can turn
// generated shader text into a loadable binary without running picasso. It reproduces picasso's output bit for bit, including the order dependent parts
// (operand descriptor sharing, the "mad descriptors must be < 32" swap, uniform allocation from both ends, padding NOPs, float24 truncation): tests compare
// it with .shbin files built by the real picasso.
//
// Not supported (reported as errors): geometry shaders (.gsh, setemit), multiple DVLEs per binary, .nodvle, the picasso header file output.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace t2p {

struct PicaBinUniform {
  std::string name;
  int pos = 0;              // register id as picasso uses it (0..0xF input, 0x20.. float vector, 0x80.. integer vector, 0x88.. bool)
  int size = 1;
  int type = 2;             // 0 bool, 1 integer vector, 2 float vector (also used for .in names)
};

struct PicaBinConstant {
  int regId = 0;            // 0x20.. float vector, 0x80.. integer vector, 0x88.. bool
  int type = 2;             // 0 bool, 1 integer vector, 2 float vector
  float f[4] = {0, 0, 0, 0};
  uint8_t i[4] = {0, 0, 0, 0};
  bool b = false;
};

struct PicaBinOutput {      // .out: property type, output register, component mask (bit0 = x)
  int type = 0, reg = 0, mask = 0;
};

struct PicaAsmResult {
  bool ok = false;
  std::string error;                       // "file:line: error: ..." of the first problem
  std::vector<std::string> warnings;       // picasso's warnings ("arbitrary swizzling has no effect ...")
  std::vector<uint8_t> shbin;              // the complete SHBIN file (4 byte aligned storage)
  // Parts, for inspection and tests
  std::vector<uint32_t> code;              // instruction words, after relocation (including padding NOPs)
  std::vector<uint32_t> opdescs;           // operand descriptors
  std::vector<PicaBinUniform> uniforms;    // .in names and referenced uniforms of the DVLE, sorted by position
  std::vector<PicaBinConstant> constants;  // .constf / .consti / .setf / .seti / .setb / constant arrays, in declaration order
  std::vector<PicaBinOutput> outputs;
  uint32_t entryStart = 0, entryEnd = 0;   // main procedure: first word, word after the last
  uint16_t inputMask = 0, outputMask = 0;
};

// Assembles one source file. `filename` only appears in messages. autoNop = picasso's padding NOPs (disable with false, like picasso's --no-nop).
PicaAsmResult assemblePica(const std::string& source, const std::string& filename = "shader.v.pica", bool autoNop = true);

// float32 -> float24 the way picasso stores constants: the mantissa is truncated, the exponent re-biased, underflow flushes to zero, overflow saturates.
uint32_t picaF32toF24(float f);
// float24 (as stored in the SHBIN constant table) -> float32, exact. Zero exponent reads as zero, 0x7F as infinity.
float picaF24toF32(uint32_t v);

}  // namespace t2p
