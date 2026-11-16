// PICA200 vertex-shader assembler (see picaenc.h). A port of devkitPro picasso 2.x (picasso_assembler.cpp, picasso_frontend.cpp): the control flow, the order of
// the operations and the bit layouts follow the original so that the output is identical, including its quirks. Differences: errors are returned instead of
// printed, there is a single input file and a single DVLE, and the geometry shader parts are left out.
#include "picaenc.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

namespace t2p {

uint32_t picaF32toF24(float f) {
  uint32_t i;
  std::memcpy(&i, &f, sizeof(f));
  uint32_t mantissa = (i << 9) >> 9;
  int32_t exponent = int32_t((i << 1) >> 24);   // unsigned shifts, like picasso (i is a uint32_t there)
  uint32_t sign = i >> 31;
  mantissa >>= 7;                               // truncate
  exponent = exponent - 127 + 63;
  if (exponent < 0) return sign << 23;          // underflow: flush to zero
  if (exponent > 0x7F) return (sign << 23) | (0x7Fu << 16);   // overflow: saturate
  return (sign << 23) | (uint32_t(exponent) << 16) | mantissa;
}

float picaF24toF32(uint32_t v) {
  uint32_t sign = (v >> 23) & 1, exp = (v >> 16) & 0x7F, man = v & 0xFFFF, b;
  if (exp == 0) b = sign << 31;
  else if (exp == 0x7F) b = (sign << 31) | (0xFFu << 23);
  else b = (sign << 31) | ((exp - 63 + 127) << 23) | (man << 7);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

namespace {

typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t u16;
typedef uint8_t u8;

enum {
  MAESTRO_ADD = 0x00, MAESTRO_DP3, MAESTRO_DP4, MAESTRO_DPH, MAESTRO_DST, MAESTRO_EX2, MAESTRO_LG2, MAESTRO_LITP, MAESTRO_MUL, MAESTRO_SGE, MAESTRO_SLT,
  MAESTRO_FLR, MAESTRO_MAX, MAESTRO_MIN, MAESTRO_RCP, MAESTRO_RSQ,
  MAESTRO_unk10, MAESTRO_unk11, MAESTRO_MOVA, MAESTRO_MOV, MAESTRO_unk14, MAESTRO_unk15, MAESTRO_unk16, MAESTRO_unk17,
  MAESTRO_DPHI, MAESTRO_DSTI, MAESTRO_SGEI, MAESTRO_SLTI, MAESTRO_unk1C, MAESTRO_unk1D, MAESTRO_unk1E, MAESTRO_unk1F,
  MAESTRO_BREAK, MAESTRO_NOP, MAESTRO_END, MAESTRO_BREAKC, MAESTRO_CALL, MAESTRO_CALLC, MAESTRO_CALLU, MAESTRO_IFU, MAESTRO_IFC, MAESTRO_FOR,
  MAESTRO_EMIT, MAESTRO_SETEMIT, MAESTRO_JMPC, MAESTRO_JMPU, MAESTRO_CMP,
  MAESTRO_MADI = 0x30, MAESTRO_MAD = 0x38,
};

enum { COMP_X = 0, COMP_Y, COMP_Z, COMP_W };
enum { COND_EQ = 0, COND_NE, COND_LT, COND_LE, COND_GT, COND_GE };
enum { SE_PROC, SE_FOR, SE_IF, SE_ARRAY };
enum { UTYPE_BOOL = 0, UTYPE_IVEC, UTYPE_FVEC };
enum { OUTTYPE_POS = 0, OUTTYPE_NQUAT = 1, OUTTYPE_CLR = 2, OUTTYPE_TCOORD0 = 3, OUTTYPE_TCOORD0W = 4, OUTTYPE_TCOORD1 = 5, OUTTYPE_TCOORD2 = 6, OUTTYPE_VIEW = 8, OUTTYPE_DUMMY = 9 };

constexpr int kMaxVshSize = 512;
constexpr int kMaxStack = 32;
constexpr int kMaxOpdesc = 128;
constexpr int kMaxUniform = 0x60;
constexpr int kMaxConstant = 0x60;
constexpr int kMaxOutput = 16;

constexpr u32 bit(int n) { return 1u << n; }
constexpr int swzComp(int n, int v) { return v << (6 - n * 2); }
constexpr int opsrcMake(int neg, int sw) { return neg | (sw << 1); }
constexpr int opdescMake(int out, int src1, int src2, int src3) { return out | (src1 << 4) | (src2 << (4 + 9)) | (src3 << (4 + 9 * 2)); }
constexpr u32 fmtOpcode(int n) { return u32(n) << 26; }
constexpr u64 outputMake(int i, int reg, int mask) { return u64(i) | (u64(reg) << 16) | (u64(mask) << 32); }
constexpr int kDefaultSwizzle = swzComp(0, COMP_X) | swzComp(1, COMP_Y) | swzComp(2, COMP_Z) | swzComp(3, COMP_W);
constexpr int kDefaultOpsrc = opsrcMake(0, kDefaultSwizzle);
constexpr int kMaskD123 = opdescMake(0xF, 0x1FF, 0x1FF, 0x1FF);
constexpr int kMaskD12 = opdescMake(0xF, 0x1FF, 0x1FF, 0);
constexpr int kMaskD1 = opdescMake(0xF, 0x1FF, 0, 0);
constexpr int kMask12 = opdescMake(0, 0x1FF, 0x1FF, 0);

struct UniformAlloc {
  int start, end, bound, tend;
  UniformAlloc(int s, int e) : start(s), end(e), bound(e), tend(e) {}
  int allocGlobal(int size) {
    if (start + size > bound) return -1;
    int ret = start;
    start += size;
    return ret;
  }
  int allocLocal(int size) {
    int pos = end - size;
    if (pos < start) return -1;
    bound = pos < bound ? pos : bound;
    end = pos;
    return pos;
  }
};

struct Uniform {
  std::string name;
  int pos = 0, size = 1, type = UTYPE_FVEC;
  void init(const char* n, int p, int s, int t) { name = n; pos = p; size = s; type = t; }
  bool operator<(const Uniform& r) const { return pos < r.pos; }
};

struct Constant {
  int regId = 0;
  int type = UTYPE_FVEC;
  float fparam[4] = {0, 0, 0, 0};
  u8 iparam[4] = {0, 0, 0, 0};
  bool bparam = false;
};

struct StackEntry {
  int type = 0;
  size_t pos = 0;
  std::string name;   // SE_PROC
  size_t uExtra = 0;  // SE_IF: position of the else part
};

typedef std::pair<size_t, std::string> Relocation;   // position, name

class Assembler {
 public:
  Assembler(const std::string& filename, bool autoNop) : curFile_(filename), autoNop_(autoNop) {}
  bool run(const std::string& source, PicaAsmResult& R);

 private:
  struct Cmd {
    const char* name;
    int (Assembler::*func)(int, int);
    int opcode, opcodei;
  };

  // ------------------------------------------------------------------ state
  std::string curFile_;
  int curLine_ = 1;
  bool autoNop_;
  std::string err_;
  std::vector<std::string> warnings_;

  std::vector<u32> buf_;
  StackEntry stack_[kMaxStack];
  int stackPos_ = 0;

  int opdescTable_[kMaxOpdesc] = {};
  int opdescMasks_[kMaxOpdesc] = {};
  int opdescCount_ = 0;
  u32 opdescIsMad_ = 0;

  UniformAlloc fvecAlloc_{0x20, 0x80}, ivecAlloc_{0x80, 0x84}, boolAlloc_{0x88, 0x98};
  Uniform uniformTable_[kMaxUniform];   // global uniform table (only the duplicate check uses it with a single file)
  int uniformCount_ = 0;

  std::vector<Constant> constArray_;
  int constArraySize_ = -1;
  std::string constArrayName_;

  std::map<std::string, std::pair<size_t, size_t>> procTable_;   // position, size
  std::map<std::string, size_t> labels_;
  std::map<std::string, int> aliases_;
  std::vector<Relocation> procRelocs_, labelRelocs_;
  bool lastWasEnd_ = false;

  // the one DVLE
  bool haveDvle_ = false;
  struct Dvle {
    std::string entrypoint = "main";
    size_t entryStart = 0, entryEnd = 0;
    u16 inputMask = 0, outputMask = 0;
    Uniform uniformTable[kMaxUniform];
    int uniformCount = 0;
    size_t symbolSize = 0;
    Constant constantTable[kMaxConstant];
    int constantCount = 0;
    u64 outputTable[kMaxOutput] = {};
    u32 outputUsedReg = 0;
    int outputCount = 0;
    int findFreeOutput() const {
      for (int i = 0; i < 0x10; i++)
        if (!(outputMask & bit(i))) return i;
      return -1;
    }
    int findFreeInput() const {
      for (int i = 0; i < 16; i++)
        if (!(inputMask & bit(i))) return i;
      return -1;
    }
  } dvle_;
  Dvle& dvle() { haveDvle_ = true; return dvle_; }

  // ------------------------------------------------------------------ text helpers (picasso uses strtok style splitting on a mutable buffer)
  char* tokPos_ = nullptr;
  char* mystrtok(char* str, const char* delim) {
    if (!str) str = tokPos_;
    if (!str || !*str) return nullptr;
    size_t pos = std::strcspn(str, delim);
    char* ret = str;
    str += pos;
    if (*str) *str++ = 0;
    tokPos_ = str;
    return ret;
  }
  char* mystrtokSpc(char* str) {
    char* ret = mystrtok(str, " \t");
    if (!ret) return nullptr;
    if (*tokPos_)
      for (; *tokPos_ && std::isspace((unsigned char)*tokPos_); tokPos_++) {}
    return ret;
  }
  static char* removeComment(char* buf) {
    char* pos = std::strchr(buf, ';');
    if (pos) *pos = 0;
    return buf;
  }
  static char* trimWhitespace(char* buf) {
    if (!buf) return nullptr;
    for (int pos = int(std::strlen(buf)) - 1; pos >= 0 && std::isspace((unsigned char)buf[pos]); pos--) buf[pos] = '\0';
    char* nb = buf;
    for (; std::isspace((unsigned char)*nb); nb++) {}
    return nb;
  }
  char* nextArg() { return trimWhitespace(mystrtok(nullptr, ",")); }
  char* nextArgCParen() { return trimWhitespace(mystrtok(nullptr, "(")); }
  char* nextArgSpc() { return trimWhitespace(mystrtokSpc(nullptr)); }

  static bool validateIdentifier(const char* id) {
    int len = int(std::strlen(id));
    bool valid = true;
    for (int i = 0; valid && i < len; i++) {
      int c = (unsigned char)id[i];
      valid = std::isalpha(c) || c == '_' || c == '$' || (i > 0 && std::isdigit(c));
    }
    return valid;
  }
  static bool stricmpEq(const char* a, const char* b) {
    for (;; a++, b++) {
      if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
      if (!*a) return true;
    }
  }

  int throwError(const char* msg, ...) {
    char tmp[512];
    va_list v;
    va_start(v, msg);
    std::vsnprintf(tmp, sizeof(tmp), msg, v);
    va_end(v);
    std::string m = tmp;
    while (!m.empty() && m.back() == '\n') m.pop_back();
    if (err_.empty()) err_ = curFile_ + ":" + std::to_string(curLine_) + ": error: " + m;
    return 1;
  }
  void warning(const char* msg) { warnings_.push_back(curFile_ + ":" + std::to_string(curLine_) + ": warning: " + msg); }

  int parseInt(const char* pos, int& out, long long mn, long long mx) {
    char* endptr = nullptr;
    long long res = std::strtoll(pos, &endptr, 0);
    if (pos == endptr) return throwError("Invalid value: %s\n", pos);
    if (res < mn || res > mx) return throwError("Value out of range (%d..%u): %d\n", (int)mn, (unsigned)mx, (int)res);
    out = int(res);
    return 0;
  }

  // ------------------------------------------------------------------ operands
  static int parseSwizzling(const char* b) {
    int i, out = 0, q = COMP_X;
    for (i = 0; b[i] && i < 4; i++) {
      switch (std::tolower((unsigned char)b[i])) {
        case 'x': case 'r': case 's': q = COMP_X; break;
        case 'y': case 'g': case 't': q = COMP_Y; break;
        case 'z': case 'b': case 'p': q = COMP_Z; break;
        case 'w': case 'a': case 'q': q = COMP_W; break;
        default: return -1;
      }
      out |= swzComp(i, q);
    }
    if (b[i]) return -1;
    for (int j = i; j < 4; j++) out |= swzComp(j, q);
    return out << 1;
  }

  int maskFromSwizzling(int sw, bool reverse = true) {
    sw >>= 1;
    int out = 0, prevbitid = 4;
    for (int i = 0; i < 4; i++) {
      int bitid = (sw >> (i * 2)) & 3;
      if (bitid > prevbitid) warning("arbitrary swizzling has no effect for destination mask");
      prevbitid = bitid;
      if (reverse) bitid = 3 - bitid;
      out |= int(bit(bitid));
    }
    return out;
  }

  static void optimizeOpdesc(int& mask, int opcode, int opdesc) {
    int unused1 = 0, unused2 = 0, unused3 = 0;
    switch (opcode) {
      case MAESTRO_ADD: case MAESTRO_MUL: case MAESTRO_SGE: case MAESTRO_SLT: case MAESTRO_FLR: case MAESTRO_MAX: case MAESTRO_MIN: case MAESTRO_MOV: case MAESTRO_MAD:
        for (int i = 0; i < 4; i++)
          if (!(opdesc & int(bit(3 - i)))) unused1 |= swzComp(i, 3);
        unused2 = unused1;
        unused3 = unused1;
        break;
      case MAESTRO_DP3:
        unused1 = swzComp(3, 3);
        unused2 = swzComp(3, 3);
        break;
      case MAESTRO_DPH:
        unused1 = swzComp(3, 3);
        break;
      case MAESTRO_EX2: case MAESTRO_LG2: case MAESTRO_RCP: case MAESTRO_RSQ:
        unused1 = swzComp(1, 3) | swzComp(2, 3) | swzComp(3, 3);
        break;
      case MAESTRO_MOVA:
        if (!(opdesc & int(bit(3 - COMP_X)))) unused1 |= swzComp(0, 3);
        if (!(opdesc & int(bit(3 - COMP_Y)))) unused1 |= swzComp(1, 3);
        // fall through
      case MAESTRO_CMP:
        unused1 |= swzComp(2, 3) | swzComp(3, 3);
        break;
    }
    mask &= ~opdescMake(0, opsrcMake(0, unused1), opsrcMake(0, unused2), opsrcMake(0, unused3));
  }

  int findOrAddOpdesc(int opcode, int& out, int opdesc, int mask) {
    optimizeOpdesc(mask, opcode, opdesc);
    for (int i = 0; i < opdescCount_; i++) {
      int minMask = mask & opdescMasks_[i];
      if ((opdesc & minMask) == (opdescTable_[i] & minMask)) {
        opdescTable_[i] = (opdescTable_[i] & ~mask) | (opdesc & mask);
        opdescMasks_[i] |= mask;
        out = i;
        return 0;
      }
    }
    if (opdescCount_ == kMaxOpdesc) return throwError("too many operand descriptors (limit is %d)\n", kMaxOpdesc);
    opdescTable_[opdescCount_] = opdesc;
    opdescMasks_[opdescCount_] = mask;
    out = opdescCount_++;
    return 0;
  }

  void swapOpdesc(u32 from, u32 to) {
    std::swap(opdescTable_[from], opdescTable_[to]);
    std::swap(opdescMasks_[from], opdescMasks_[to]);
    for (size_t i = 0; i < buf_.size(); i++) {
      u32& opword = buf_[i];
      u32 opcode = opword >> 26;
      if (opcode < 0x20 || (opcode & ~1u) == MAESTRO_CMP) {
        u32 cur = opword & 0x7F;
        if (cur == from) cur = to;
        else if (cur == to) cur = from;
        opword = (opword & ~0x7Fu) | cur;
      }
    }
  }

  static bool isregp(int x) {
    x = std::tolower(x);
    return x == 'o' || x == 'v' || x == 'r' || x == 'c' || x == 'i' || x == 'b';
  }
  static int convertIdxRegName(const char* reg) {
    if (stricmpEq(reg, "a0") || stricmpEq(reg, "a0.x")) return 1;
    if (stricmpEq(reg, "a1") || stricmpEq(reg, "a0.y")) return 2;
    if (stricmpEq(reg, "a2") || stricmpEq(reg, "lcnt") || stricmpEq(reg, "aL")) return 3;
    return 0;
  }
  static int parseCondOp(const char* name) {
    if (stricmpEq(name, "eq")) return COND_EQ;
    if (stricmpEq(name, "ne")) return COND_NE;
    if (stricmpEq(name, "lt")) return COND_LT;
    if (stricmpEq(name, "le")) return COND_LE;
    if (stricmpEq(name, "gt")) return COND_GT;
    if (stricmpEq(name, "ge")) return COND_GE;
    return -1;
  }

#define SAFE(x) do { int _r = (x); if (_r != 0) return _r; } while (0)

  int parseReg(char* pos, int& outReg, int& outSw, int* idxType = nullptr) {
    outReg = 0;
    outSw = kDefaultOpsrc;
    if (idxType) *idxType = 0;
    if (*pos == '-') {
      pos++;
      outSw |= 1;
    }
    int regOffset = 0;
    char* offPos = std::strchr(pos, '[');
    char* dotPos = pos;
    if (offPos) {
      dotPos = std::strchr(offPos, ']');
      if (!dotPos) return throwError("missing closing bracket: %s\n", pos);
      *dotPos++ = 0;
      *offPos++ = 0;
      offPos = trimWhitespace(offPos);

      int temp = convertIdxRegName(offPos);
      if (temp > 0) {
        if (!idxType) return throwError("index register not allowed here: %s\n", offPos);
        *idxType = temp;
      } else do {
        char* plusPos = std::strchr(offPos, '+');
        if (!plusPos) break;
        if (!idxType) return throwError("index register not allowed here: %s\n", offPos);
        *plusPos++ = 0;
        char* idxRegName = trimWhitespace(offPos);
        offPos = trimWhitespace(plusPos);
        *idxType = convertIdxRegName(idxRegName);
        if (!*idxType) return throwError("invalid index register: %s\n", idxRegName);
      } while (0);

      regOffset = std::atoi(offPos);
      if (regOffset < 0) return throwError("invalid register offset: %s\n", offPos);
    }
    dotPos = std::strchr(dotPos, '.');
    if (dotPos) {
      *dotPos++ = 0;
      outSw = parseSwizzling(dotPos) | (outSw & 1);
      if (outSw < 0) return throwError("invalid swizzling mask: %s\n", dotPos);
    }
    auto it = aliases_.find(pos);
    if (it != aliases_.end()) {
      int x = it->second;
      outReg = x & 0xFF;
      outReg += regOffset;
      outSw ^= (x >> 8) & 1;
      x >>= 9;
      int temp = outSw & 1;
      for (int j = 0; j < 4; j++) {
        int comp = (outSw >> (7 - j * 2)) & 3;
        comp = (x >> (6 - comp * 2)) & 3;
        temp |= swzComp(j, comp) << 1;
      }
      outSw = temp;
      return 0;
    }

    if (!isregp((unsigned char)pos[0]) || !std::isdigit((unsigned char)pos[1])) return throwError("invalid register: %s\n", pos);

    SAFE(parseInt(pos + 1, outReg, 0, 255));
    switch (*pos) {
      case 'o':
        if (outReg < 0x00 || outReg >= 0x10) return throwError("invalid output register: %s\n", pos);
        break;
      case 'v':
        if (outReg < 0x00 || outReg >= 0x0F) return throwError("invalid input register: %s\n", pos);
        break;
      case 'r':
        outReg += 0x10;
        if (outReg < 0x10 || outReg >= 0x20) return throwError("invalid temporary register: %s\n", pos);
        break;
      case 'c':
        outReg += 0x20;
        if (outReg < 0x20 || outReg >= 0x80) return throwError("invalid floating-point vector uniform register: %s\n", pos);
        break;
      case 'i':
        outReg += 0x80;
        if (outReg < 0x80 || outReg >= 0x88) return throwError("invalid integer vector uniform register: %s\n", pos);
        break;
      case 'b':
        outReg += 0x88;
        if (outReg < 0x88 || outReg >= 0x98) return throwError("invalid boolean uniform register: %s\n", pos);
        break;
    }
    if (idxType && *idxType && (outReg < 0x20 || outReg >= 0x80)) return throwError("index register not allowed with this kind of register\n");
    outReg += regOffset;
    return 0;
  }

  int parseCondExpOp(char* str, u32& outFlags, int& which) {
    int negation = 0;
    for (; *str == '!'; str++) negation ^= 1;
    if (stricmpEq(str, "cmp.x")) {
      which = 0;
      outFlags ^= u32(negation) << 25;
      return 0;
    }
    if (stricmpEq(str, "cmp.y")) {
      which = 1;
      outFlags ^= u32(negation) << 24;
      return 0;
    }
    return throwError("invalid condition register: %s\n", str);
  }

  int parseCondExp(char* str, u32& outFlags) {
    outFlags = bit(24) | bit(25);
    size_t len = std::strlen(str);
    size_t pos = std::strcspn(str, "&|");
    int op2 = -1;
    if (pos < len) {
      char* str2 = str + pos;
      int type = *str2;
      *str2++ = 0;
      if (*str2 == type) str2++;
      str = trimWhitespace(str);
      str2 = trimWhitespace(str2);
      if (type == '&') outFlags |= 1u << 22;
      SAFE(parseCondExpOp(str2, outFlags, op2));
    }
    int op1 = -1;
    SAFE(parseCondExpOp(str, outFlags, op1));
    if (op1 == op2) return throwError("condition register checked twice\n");
    if (op2 < 0) outFlags |= u32(op1 + 2) << 22;
    return 0;
  }

  static bool isBadInputRegCombination(int a, int b) { return a < 0x10 && b < 0x10 && a != b; }
  static bool isBadInputRegCombination(int a, int b, int c) { return isBadInputRegCombination(a, b) || isBadInputRegCombination(b, c) || isBadInputRegCombination(c, a); }

  void insertPaddingNop() {
    if (autoNop_) buf_.push_back(fmtOpcode(MAESTRO_NOP));
    else warning("a padding NOP is required here");
  }

  int ensureNoMoreArgs() { return nextArg() ? throwError("too many parameters\n") : 0; }
  int missingParam() { return throwError("missing parameter\n"); }
  int duplicateIdentifier(const char* id) { return throwError("identifier already used: %s\n", id); }
  int ensureTarget(const char* t) { return validateIdentifier(t) ? 0 : throwError("invalid target: %s\n", t); }
  int ensureValidDest(int reg, const char* name) { return (reg < 0x00 || reg >= 0x20) ? throwError("invalid destination register: %s\n", name) : 0; }
  int ensureValidSrcWide(int reg, const char* name, int srcId) { return (reg < 0x00 || reg >= 0x80) ? throwError("invalid source%d register: %s\n", srcId, name) : 0; }
  int ensureValidSrcNarrow(int reg, const char* name, int srcId) { return (reg < 0x00 || reg >= 0x20) ? throwError("invalid source%d register: %s\n", srcId, name) : 0; }
  int ensureNoIdxreg(int idxreg, int srcId) { return idxreg > 0 ? throwError("index register not allowed in source%d\n", srcId) : 0; }
  int ensureValidIreg(int reg, const char* name) { return (reg < 0x80 || reg >= 0x88) ? throwError("invalid integer vector uniform: %s\n", name) : 0; }
  int ensureValidBreg(int reg, const char* name) { return (reg < 0x88 || reg >= 0x98) ? throwError("invalid boolean uniform: %s\n", name) : 0; }
  int ensureValidCondop(int condop, const char* name) { return condop < 0 ? throwError("invalid conditional operator: %s\n", name) : 0; }

#define NEXT_ARG(v) char* v = nextArg(); if (!v) return missingParam()
#define NEXT_ARG_SPC(v) char* v = nextArgSpc(); if (!v) return missingParam()
#define NEXT_ARG_CPAREN(v) char* v = nextArgCParen(); if (!v) return missingParam()
#define ENSURE_NO_MORE_ARGS() SAFE(ensureNoMoreArgs())
#define ARG_TO_REG(v, a) int v = 0, v##Sw = 0; SAFE(parseReg(a, v, v##Sw))
#define ARG_TO_REG2(v, a) int v = 0, v##Sw = 0, v##Idx = 0; SAFE(parseReg(a, v, v##Sw, &v##Idx))
#define ARG_TO_INT(v, a, mn, mx) int v = 0; SAFE(parseInt(a, v, mn, mx))
#define ARG_TO_DEST_REG(r, n) ARG_TO_REG(r, n); SAFE(ensureValidDest(r, n))
#define ARG_TO_SRC1_REG2(r, n) ARG_TO_REG2(r, n); SAFE(ensureValidSrcWide(r, n, 1))
#define ARG_TO_SRC2_REG(r, n) ARG_TO_REG(r, n); SAFE(ensureValidSrcNarrow(r, n, 2))
#define ARG_TO_IREG(r, n) ARG_TO_REG(r, n); SAFE(ensureValidIreg(r, n))
#define ARG_TO_BREG(r, n) ARG_TO_REG(r, n); SAFE(ensureValidBreg(r, n))
#define ARG_TO_CONDOP(v, a) int v = parseCondOp(a); SAFE(ensureValidCondop(v, a))

  // ------------------------------------------------------------------ instructions
  int cmd_format0(int opcode, int) {
    ENSURE_NO_MORE_ARGS();
    buf_.push_back(fmtOpcode(opcode));
    return 0;
  }

  int cmd_format1(int opcode, int opcodei) {
    NEXT_ARG(destName);
    NEXT_ARG(src1Name);
    NEXT_ARG(src2Name);
    ENSURE_NO_MORE_ARGS();

    ARG_TO_DEST_REG(rDest, destName);
    ARG_TO_REG2(rSrc1, src1Name);
    ARG_TO_REG2(rSrc2, src2Name);

    bool inverted = opcodei >= 0 && rSrc1 < 0x20 && rSrc2 >= 0x20;
    if (!inverted) {
      SAFE(ensureValidSrcWide(rSrc1, src1Name, 1));
      SAFE(ensureValidSrcNarrow(rSrc2, src2Name, 2));
      SAFE(ensureNoIdxreg(rSrc2Idx, 2));
    } else {
      SAFE(ensureValidSrcNarrow(rSrc1, src1Name, 1));
      SAFE(ensureNoIdxreg(rSrc1Idx, 1));
      SAFE(ensureValidSrcWide(rSrc2, src2Name, 2));
    }
    if (isBadInputRegCombination(rSrc1, rSrc2)) return throwError("source operands must be different input registers (v0..v15)\n");

    int opdesc = 0;
    SAFE(findOrAddOpdesc(opcode, opdesc, opdescMake(maskFromSwizzling(rDestSw), rSrc1Sw, rSrc2Sw, 0), kMaskD12));
    if (!inverted)
      buf_.push_back(fmtOpcode(opcode) | u32(opdesc) | (u32(rSrc2) << 7) | (u32(rSrc1) << 12) | (u32(rSrc1Idx) << 19) | (u32(rDest) << 21));
    else
      buf_.push_back(fmtOpcode(opcodei) | u32(opdesc) | (u32(rSrc2) << 7) | (u32(rSrc1) << 14) | (u32(rSrc2Idx) << 19) | (u32(rDest) << 21));
    return 0;
  }

  int cmd_format1u(int opcode, int) {
    NEXT_ARG(destName);
    NEXT_ARG(src1Name);
    ENSURE_NO_MORE_ARGS();

    ARG_TO_DEST_REG(rDest, destName);
    ARG_TO_SRC1_REG2(rSrc1, src1Name);

    int opdesc = 0;
    SAFE(findOrAddOpdesc(opcode, opdesc, opdescMake(maskFromSwizzling(rDestSw), rSrc1Sw, 0, 0), kMaskD1));
    buf_.push_back(fmtOpcode(opcode) | u32(opdesc) | (u32(rSrc1) << 12) | (u32(rSrc1Idx) << 19) | (u32(rDest) << 21));
    return 0;
  }

  int cmd_format1c(int opcode, int) {
    NEXT_ARG(src1Name);
    NEXT_ARG(cmpxName);
    NEXT_ARG(cmpyName);
    NEXT_ARG(src2Name);
    ENSURE_NO_MORE_ARGS();

    ARG_TO_SRC1_REG2(rSrc1, src1Name);
    ARG_TO_CONDOP(cmpx, cmpxName);
    ARG_TO_CONDOP(cmpy, cmpyName);
    ARG_TO_SRC2_REG(rSrc2, src2Name);

    int opdesc = 0;
    SAFE(findOrAddOpdesc(opcode, opdesc, opdescMake(0, rSrc1Sw, rSrc2Sw, 0), kMask12));
    buf_.push_back(fmtOpcode(opcode) | u32(opdesc) | (u32(rSrc2) << 7) | (u32(rSrc1) << 12) | (u32(rSrc1Idx) << 19) | (u32(cmpy) << 21) | (u32(cmpx) << 24));
    return 0;
  }

  int cmd_format5(int opcode, int opcodei) {
    NEXT_ARG(destName);
    NEXT_ARG(src1Name);
    NEXT_ARG(src2Name);
    NEXT_ARG(src3Name);
    ENSURE_NO_MORE_ARGS();

    ARG_TO_DEST_REG(rDest, destName);
    ARG_TO_SRC2_REG(rSrc1, src1Name);
    ARG_TO_REG2(rSrc2, src2Name);
    ARG_TO_REG2(rSrc3, src3Name);

    bool inverted = opcodei >= 0 && rSrc2 < 0x20 && (rSrc3 >= 0x20 || (rSrc3Idx && !rSrc2Idx));
    if (!inverted) {
      SAFE(ensureValidSrcWide(rSrc2, src2Name, 2));
      SAFE(ensureValidSrcNarrow(rSrc3, src3Name, 3));
      SAFE(ensureNoIdxreg(rSrc3Idx, 2));
    } else {
      SAFE(ensureValidSrcNarrow(rSrc2, src2Name, 2));
      SAFE(ensureValidSrcWide(rSrc3, src3Name, 3));
      SAFE(ensureNoIdxreg(rSrc2Idx, 2));
    }
    if (isBadInputRegCombination(rSrc1, rSrc2, rSrc3)) return throwError("source registers must be different input registers (v0..v15)\n");

    int opdesc = 0;
    SAFE(findOrAddOpdesc(opcode, opdesc, opdescMake(maskFromSwizzling(rDestSw), rSrc1Sw, rSrc2Sw, rSrc3Sw), kMaskD123));

    if (opdesc >= 32) {
      int which;
      for (which = 0; which < 32; which++)
        if (!(opdescIsMad_ & bit(which))) break;
      if (which == 32) return throwError("opdesc allocation error\n");
      swapOpdesc(u32(which), u32(opdesc));
      opdesc = which;
    }
    opdescIsMad_ |= bit(opdesc);

    if (!inverted)
      buf_.push_back(fmtOpcode(opcode) | u32(opdesc) | (u32(rSrc3) << 5) | (u32(rSrc2) << 10) | (u32(rSrc1) << 17) | (u32(rSrc2Idx) << 22) | (u32(rDest) << 24));
    else
      buf_.push_back(fmtOpcode(opcodei) | u32(opdesc) | (u32(rSrc3) << 5) | (u32(rSrc2) << 12) | (u32(rSrc1) << 17) | (u32(rSrc3Idx) << 22) | (u32(rDest) << 24));
    return 0;
  }

  int cmd_formatmova(int opcode, int) {
    NEXT_ARG(targetReg);
    NEXT_ARG(src1Name);
    ENSURE_NO_MORE_ARGS();

    int mask;
    if (stricmpEq(targetReg, "a0") || stricmpEq(targetReg, "a0.x")) mask = int(bit(3));
    else if (stricmpEq(targetReg, "a1") || stricmpEq(targetReg, "a0.y")) mask = int(bit(2));
    else if (stricmpEq(targetReg, "a01") || stricmpEq(targetReg, "a0.xy")) mask = int(bit(3) | bit(2));
    else return throwError("invalid destination register for mova: %s\n", targetReg);

    ARG_TO_SRC1_REG2(rSrc1, src1Name);

    int opdesc = 0;
    SAFE(findOrAddOpdesc(opcode, opdesc, opdescMake(mask, rSrc1Sw, 0, 0), kMaskD1));
    buf_.push_back(fmtOpcode(opcode) | u32(opdesc) | (u32(rSrc1) << 12) | (u32(rSrc1Idx) << 19));
    return 0;
  }

  int cmd_unsupported(int, int) { return throwError("geometry shader instructions are not supported\n"); }

  int cmd_formatcall(int opcode, int) {
    NEXT_ARG(procName);
    ENSURE_NO_MORE_ARGS();
    SAFE(ensureTarget(procName));
    procRelocs_.push_back(std::make_pair(buf_.size(), std::string(procName)));
    buf_.push_back(fmtOpcode(opcode));
    return 0;
  }

  int cmd_formatfor(int opcode, int) {
    NEXT_ARG(regName);
    ENSURE_NO_MORE_ARGS();
    ARG_TO_IREG(regId, regName);
    if (stackPos_ == kMaxStack) return throwError("too many nested blocks\n");
    StackEntry& elem = stack_[stackPos_++];
    elem = StackEntry();
    elem.type = SE_FOR;
    elem.pos = buf_.size();
    buf_.push_back(fmtOpcode(opcode) | (u32(regId - 0x80) << 22));
    return 0;
  }

  int cmd_format2(int opcode, int) {
    NEXT_ARG(condExp);
    u32 instruction = 0;
    SAFE(parseCondExp(condExp, instruction));

    switch (opcode) {
      case MAESTRO_BREAKC:
        ENSURE_NO_MORE_ARGS();
        break;
      case MAESTRO_CALLC:
      case MAESTRO_JMPC: {
        NEXT_ARG(targetName);
        ENSURE_NO_MORE_ARGS();
        SAFE(ensureTarget(targetName));
        auto& rt = opcode == MAESTRO_CALLC ? procRelocs_ : labelRelocs_;
        rt.push_back(std::make_pair(buf_.size(), std::string(targetName)));
        break;
      }
      case MAESTRO_IFC: {
        ENSURE_NO_MORE_ARGS();
        if (stackPos_ == kMaxStack) return throwError("too many nested blocks\n");
        StackEntry& elem = stack_[stackPos_++];
        elem = StackEntry();
        elem.type = SE_IF;
        elem.pos = buf_.size();
        elem.uExtra = 0;
        break;
      }
    }
    buf_.push_back(fmtOpcode(opcode) | instruction);
    return 0;
  }

  int cmd_format3(int opcode, int) {
    NEXT_ARG(regName);
    u32 negation = 0;
    if (*regName == '!') {
      if (opcode == MAESTRO_JMPU) {
        negation = 1;
        regName++;
      } else {
        return throwError("Inverting the condition is not supported by %s\n", opcode == MAESTRO_CALLU ? "CALLU" : "IFU");
      }
    }
    ARG_TO_BREG(regId, regName);

    switch (opcode) {
      case MAESTRO_CALLU:
      case MAESTRO_JMPU: {
        NEXT_ARG(targetName);
        ENSURE_NO_MORE_ARGS();
        SAFE(ensureTarget(targetName));
        auto& rt = opcode == MAESTRO_CALLU ? procRelocs_ : labelRelocs_;
        rt.push_back(std::make_pair(buf_.size(), std::string(targetName)));
        break;
      }
      case MAESTRO_IFU: {
        ENSURE_NO_MORE_ARGS();
        if (stackPos_ == kMaxStack) return throwError("too many nested blocks\n");
        StackEntry& elem = stack_[stackPos_++];
        elem = StackEntry();
        elem.type = SE_IF;
        elem.pos = buf_.size();
        elem.uExtra = 0;
        break;
      }
    }
    buf_.push_back(fmtOpcode(opcode) | (u32(regId - 0x88) << 22) | negation);
    return 0;
  }

  // ------------------------------------------------------------------ directives
  int dir_proc(int, int) {
    NEXT_ARG(procName);
    ENSURE_NO_MORE_ARGS();
    if (stackPos_ == kMaxStack) return throwError("too many nested blocks\n");
    StackEntry& elem = stack_[stackPos_++];
    elem = StackEntry();
    elem.type = SE_PROC;
    elem.pos = buf_.size();
    elem.name = procName;
    if (procTable_.find(procName) != procTable_.end()) return throwError("proc already exists: %s\n", procName);
    return 0;
  }

  int dir_else(int, int) {
    ENSURE_NO_MORE_ARGS();
    if (!stackPos_) return throwError(".else with unmatched IF\n");
    StackEntry& elem = stack_[stackPos_ - 1];
    if (elem.type != SE_IF) return throwError(".else with unmatched IF\n");
    if (elem.uExtra) return throwError("spurious .else\n");

    if (lastWasEnd_) {
      insertPaddingNop();
      lastWasEnd_ = false;
    } else {
      u32 p = u32(buf_.size());
      u32 lastOpcode = buf_[p - 1] >> 26;
      if (lastOpcode == MAESTRO_JMPC || lastOpcode == MAESTRO_JMPU || lastOpcode == MAESTRO_CALL || lastOpcode == MAESTRO_CALLC || lastOpcode == MAESTRO_CALLU ||
          (p - elem.pos) < 2)
        insertPaddingNop();
    }
    u32 curPos = u32(buf_.size());
    elem.uExtra = curPos;
    u32& inst = buf_[elem.pos];
    inst &= ~(0xFFFu << 10);
    inst |= curPos << 10;
    return 0;
  }

  int dir_end(int, int) {
    ENSURE_NO_MORE_ARGS();
    if (!stackPos_) return throwError(".end with unmatched block\n");
    StackEntry elem = stack_[--stackPos_];

    if (elem.type != SE_ARRAY && lastWasEnd_) {
      insertPaddingNop();
      lastWasEnd_ = false;
    } else if ((elem.type == SE_PROC || elem.type == SE_FOR || elem.type == SE_IF) && buf_.size() > 0) {
      u32 p = u32(buf_.size());
      u32 lastOpcode = buf_[p - 1] >> 26;
      if (lastOpcode == MAESTRO_JMPC || lastOpcode == MAESTRO_JMPU || lastOpcode == MAESTRO_CALL || lastOpcode == MAESTRO_CALLC || lastOpcode == MAESTRO_CALLU ||
          (elem.type == SE_FOR && (lastOpcode == MAESTRO_BREAK || lastOpcode == MAESTRO_BREAKC)) ||
          (elem.type != SE_ARRAY && (p - elem.pos) < u32(elem.type != SE_PROC ? 2 : 1)))
        insertPaddingNop();
    }

    u32 curPos = u32(buf_.size());
    u32 size = curPos - u32(elem.pos);

    switch (elem.type) {
      case SE_PROC:
        procTable_.insert(std::make_pair(elem.name, std::make_pair(elem.pos, size_t(size))));
        break;
      case SE_FOR: {
        u32& inst = buf_[elem.pos];
        inst &= ~(0xFFFu << 10);
        inst |= (curPos - 1) << 10;
        lastWasEnd_ = true;
        break;
      }
      case SE_IF: {
        u32& inst = buf_[elem.pos];
        if (!elem.uExtra) {
          inst &= ~(0xFFFu << 10);
          inst |= curPos << 10;
        } else {
          inst &= ~0x3FFu;
          inst |= curPos - u32(elem.uExtra);
        }
        lastWasEnd_ = true;
        break;
      }
      case SE_ARRAY: {
        Dvle& d = dvle();
        UniformAlloc& alloc = fvecAlloc_;
        if (aliases_.find(constArrayName_) != aliases_.end()) return duplicateIdentifier(constArrayName_.c_str());

        int size2 = int(constArray_.size());
        if (constArraySize_ >= 0)
          for (; size2 < constArraySize_; size2++) {
            Constant c;
            c.type = UTYPE_FVEC;
            constArray_.push_back(c);
          }
        if (size2 == 0) return throwError("no elements have been specified in array '%s'\n", constArrayName_.c_str());

        int uniformPos = alloc.allocLocal(size2);
        if (uniformPos < 0) return throwError("not enough space for local constant array '%s'\n", constArrayName_.c_str());
        if (d.constantCount + size2 > kMaxConstant) return throwError("too many local constants\n");

        for (int i = 0; i < size2; i++) {
          Constant& src = constArray_[i];
          Constant& dst = d.constantTable[d.constantCount++];
          src.regId = uniformPos + i;
          dst = src;
        }
        aliases_.insert(std::make_pair(constArrayName_, uniformPos | (kDefaultOpsrc << 8)));
        constArray_.clear();
        constArraySize_ = -1;
        constArrayName_.clear();
        break;
      }
    }
    return 0;
  }

  int dir_alias(int, int) {
    NEXT_ARG_SPC(aliasName);
    NEXT_ARG_SPC(aliasReg);
    ENSURE_NO_MORE_ARGS();
    if (!validateIdentifier(aliasName)) return throwError("invalid alias name: %s\n", aliasName);
    if (isregp((unsigned char)aliasName[0]) && std::isdigit((unsigned char)aliasName[1])) return throwError("cannot redefine register\n");
    ARG_TO_REG(rAlias, aliasReg);
    if (aliases_.find(aliasName) != aliases_.end()) return duplicateIdentifier(aliasName);
    aliases_.insert(std::make_pair(std::string(aliasName), rAlias | (rAliasSw << 8)));
    return 0;
  }

  UniformAlloc& getAlloc(int type) {
    switch (type) {
      default:
      case UTYPE_FVEC: return fvecAlloc_;
      case UTYPE_IVEC: return ivecAlloc_;
      case UTYPE_BOOL: return boolAlloc_;
    }
  }

  int dir_uniform(int dirParam, int) {
    Dvle& d = dvle();
    UniformAlloc& alloc = getAlloc(dirParam);
    for (;;) {
      char* argText = nextArg();
      if (!argText) break;

      int uSize = 1;
      char* sizePos = std::strchr(argText, '[');
      if (sizePos) {
        char* closePos = std::strchr(sizePos, ']');
        if (!closePos) return throwError("missing closing bracket: %s\n", argText);
        *closePos = 0;
        *sizePos++ = 0;
        sizePos = trimWhitespace(sizePos);
        uSize = std::atoi(sizePos);
        if (uSize < 1) return throwError("invalid uniform size: %s[%s]\n", argText, sizePos);
      }
      if (!validateIdentifier(argText)) return throwError("invalid uniform name: %s\n", argText);
      if (aliases_.find(argText) != aliases_.end()) return duplicateIdentifier(argText);

      int uniformPos = -1;
      for (int i = 0; i < uniformCount_; i++) {
        Uniform& u = uniformTable_[i];
        if (u.name == argText) {
          if (u.type != dirParam) return throwError("mismatched uniform type: %s\n", argText);
          if (u.size != uSize) return throwError("uniform '%s' previously declared as having size %d\n", argText, u.size);
          uniformPos = u.pos;
          break;
        }
      }
      if (uniformPos < 0) {
        if (uniformCount_ == kMaxUniform) return throwError("too many global uniforms: %s\n", argText);
        uniformPos = alloc.allocGlobal(uSize);
        if (uniformPos < 0) return throwError("not enough uniform space: %s[%d]\n", argText, uSize);
      }
      uniformTable_[uniformCount_++].init(argText, uniformPos, uSize, dirParam);

      if (*argText != '_') {
        if (d.uniformCount == kMaxUniform) return throwError("too many referenced uniforms: %s\n", argText);
        d.uniformTable[d.uniformCount++].init(argText, uniformPos, uSize, dirParam);
        d.symbolSize += std::strlen(argText) + 1;
      }
      aliases_.insert(std::make_pair(std::string(argText), uniformPos | (kDefaultOpsrc << 8)));
    }
    return 0;
  }

  int dir_const(int dirParam, int) {
    Dvle& d = dvle();
    UniformAlloc& alloc = getAlloc(dirParam);

    NEXT_ARG_CPAREN(constName);
    NEXT_ARG(arg0Text);
    NEXT_ARG(arg1Text);
    NEXT_ARG(arg2Text);
    char* arg3Text = tokPos_;
    if (!tokPos_) return missingParam();
    char* parenPos = std::strchr(arg3Text, ')');
    if (!parenPos) return throwError("invalid syntax\n");
    *parenPos = 0;
    arg3Text = trimWhitespace(arg3Text);

    if (aliases_.find(constName) != aliases_.end()) return duplicateIdentifier(constName);
    int uniformPos = alloc.allocLocal(1);
    if (uniformPos < 0) return throwError("not enough space for local constant '%s'\n", constName);
    if (d.constantCount == kMaxConstant) return throwError("too many local constants\n");

    Constant& ct = d.constantTable[d.constantCount++];
    ct.regId = uniformPos;
    ct.type = dirParam;
    if (dirParam == UTYPE_FVEC) {
      ct.fparam[0] = float(std::atof(arg0Text));
      ct.fparam[1] = float(std::atof(arg1Text));
      ct.fparam[2] = float(std::atof(arg2Text));
      ct.fparam[3] = float(std::atof(arg3Text));
    } else if (dirParam == UTYPE_IVEC) {
      ct.iparam[0] = u8(std::atoi(arg0Text) & 0xFF);
      ct.iparam[1] = u8(std::atoi(arg1Text) & 0xFF);
      ct.iparam[2] = u8(std::atoi(arg2Text) & 0xFF);
      ct.iparam[3] = u8(std::atoi(arg3Text) & 0xFF);
    }
    aliases_.insert(std::make_pair(std::string(constName), ct.regId | (kDefaultOpsrc << 8)));
    return 0;
  }

  int dir_constfa(int, int) {
    bool inArray = stackPos_ && stack_[stackPos_ - 1].type == SE_ARRAY;
    if (!inArray) {
      NEXT_ARG(constName);
      ENSURE_NO_MORE_ARGS();
      if (stackPos_ == kMaxStack) return throwError("too many nested blocks\n");

      char* sizePos = std::strchr(constName, '[');
      if (!sizePos) return throwError("missing opening bracket: %s\n", constName);
      char* closePos = std::strchr(sizePos, ']');
      if (!closePos) return throwError("missing closing bracket: %s\n", constName);
      *closePos++ = 0;
      *sizePos++ = 0;
      closePos = trimWhitespace(closePos);
      sizePos = trimWhitespace(sizePos);
      if (*closePos) return throwError("garbage found: %s\n", closePos);
      if (*sizePos) {
        constArraySize_ = std::atoi(sizePos);
        if (constArraySize_ <= 0) return throwError("invalid array size: %s[%s]\n", constName, sizePos);
      }
      if (!validateIdentifier(constName)) return throwError("invalid array name: %s\n", constName);
      constArrayName_ = constName;
      StackEntry& elem = stack_[stackPos_++];
      elem = StackEntry();
      elem.type = SE_ARRAY;
    } else {
      if (constArraySize_ >= 0 && constArraySize_ == int(constArray_.size())) return throwError("too many elements in the array, expected %d\n", constArraySize_);
      NEXT_ARG(arg0Text);
      if (*arg0Text != '(') return throwError("invalid syntax\n");
      arg0Text++;
      NEXT_ARG(arg1Text);
      NEXT_ARG(arg2Text);
      char* arg3Text = tokPos_;
      if (!tokPos_) return missingParam();
      char* parenPos = std::strchr(arg3Text, ')');
      if (!parenPos) return throwError("invalid syntax\n");
      *parenPos = 0;
      arg3Text = trimWhitespace(arg3Text);

      Constant ct;
      ct.type = UTYPE_FVEC;
      ct.fparam[0] = float(std::atof(arg0Text));
      ct.fparam[1] = float(std::atof(arg1Text));
      ct.fparam[2] = float(std::atof(arg2Text));
      ct.fparam[3] = float(std::atof(arg3Text));
      constArray_.push_back(ct);
    }
    return 0;
  }

  int dir_setfi(int dirParam, int) {
    Dvle& d = dvle();
    NEXT_ARG_CPAREN(constName);
    NEXT_ARG(arg0Text);
    NEXT_ARG(arg1Text);
    NEXT_ARG(arg2Text);
    char* arg3Text = tokPos_;
    if (!tokPos_) return missingParam();
    char* parenPos = std::strchr(arg3Text, ')');
    if (!parenPos) return throwError("invalid syntax\n");
    *parenPos = 0;
    arg3Text = trimWhitespace(arg3Text);

    ARG_TO_REG(constReg, constName);
    if (dirParam == UTYPE_FVEC) {
      if (constReg < 0x20 || constReg >= 0x80) return throwError("invalid floating point vector uniform: %s\n", constName);
    } else if (dirParam == UTYPE_IVEC) {
      if (constReg < 0x80 || constReg >= 0x84) return throwError("invalid integer vector uniform: %s\n", constName);
    }
    if (d.constantCount == kMaxConstant) return throwError("too many local constants\n");

    Constant& ct = d.constantTable[d.constantCount++];
    ct.regId = constReg;
    ct.type = dirParam;
    if (dirParam == UTYPE_FVEC) {
      ct.fparam[0] = float(std::atof(arg0Text));
      ct.fparam[1] = float(std::atof(arg1Text));
      ct.fparam[2] = float(std::atof(arg2Text));
      ct.fparam[3] = float(std::atof(arg3Text));
    } else if (dirParam == UTYPE_IVEC) {
      ct.iparam[0] = u8(std::atoi(arg0Text) & 0xFF);
      ct.iparam[1] = u8(std::atoi(arg1Text) & 0xFF);
      ct.iparam[2] = u8(std::atoi(arg2Text) & 0xFF);
      ct.iparam[3] = u8(std::atoi(arg3Text) & 0xFF);
    }
    return 0;
  }

  int parseBool(bool& out, const char* text) {
    if (stricmpEq(text, "true") || stricmpEq(text, "on") || stricmpEq(text, "1")) { out = true; return 0; }
    if (stricmpEq(text, "false") || stricmpEq(text, "off") || stricmpEq(text, "0")) { out = false; return 0; }
    return throwError("invalid bool value: %s\n", text);
  }

  int dir_setb(int, int) {
    Dvle& d = dvle();
    NEXT_ARG_SPC(constName);
    NEXT_ARG_SPC(valueText);
    ENSURE_NO_MORE_ARGS();
    ARG_TO_BREG(constReg, constName);
    bool constVal = false;
    SAFE(parseBool(constVal, valueText));
    if (d.constantCount == kMaxConstant) return throwError("too many local constants\n");
    Constant& ct = d.constantTable[d.constantCount++];
    ct.regId = constReg;
    ct.type = UTYPE_BOOL;
    ct.bparam = constVal;
    return 0;
  }

  static int parseOutType(const char* text) {
    if (stricmpEq(text, "pos") || stricmpEq(text, "position")) return OUTTYPE_POS;
    if (stricmpEq(text, "nquat") || stricmpEq(text, "normalquat")) return OUTTYPE_NQUAT;
    if (stricmpEq(text, "clr") || stricmpEq(text, "color")) return OUTTYPE_CLR;
    if (stricmpEq(text, "tcoord0") || stricmpEq(text, "texcoord0")) return OUTTYPE_TCOORD0;
    if (stricmpEq(text, "tcoord0w") || stricmpEq(text, "texcoord0w")) return OUTTYPE_TCOORD0W;
    if (stricmpEq(text, "tcoord1") || stricmpEq(text, "texcoord1")) return OUTTYPE_TCOORD1;
    if (stricmpEq(text, "tcoord2") || stricmpEq(text, "texcoord2")) return OUTTYPE_TCOORD2;
    if (stricmpEq(text, "view")) return OUTTYPE_VIEW;
    if (stricmpEq(text, "dummy")) return OUTTYPE_DUMMY;
    return -1;
  }

  int dir_in(int, int) {
    Dvle& d = dvle();
    NEXT_ARG_SPC(inName);
    char* inRegName = nextArgSpc();
    ENSURE_NO_MORE_ARGS();

    if (!validateIdentifier(inName)) return throwError("invalid identifier: %s\n", inName);
    if (aliases_.find(inName) != aliases_.end()) return duplicateIdentifier(inName);

    int oid = -1;
    if (inRegName) {
      ARG_TO_REG(inReg, inRegName);
      if (inReg < 0x00 || inReg >= 0x10) return throwError("invalid input register: %s\n", inRegName);
      oid = inReg;
    } else {
      oid = d.findFreeInput();
    }
    if (oid < 0) return throwError("too many inputs\n");
    if (d.uniformCount == kMaxUniform) return throwError("too many uniforms in DVLE\n");

    d.inputMask |= u16(bit(oid));
    d.uniformTable[d.uniformCount++].init(inName, oid, 1, UTYPE_FVEC);
    d.symbolSize += std::strlen(inName) + 1;
    aliases_.insert(std::make_pair(std::string(inName), oid | (kDefaultOpsrc << 8)));
    return 0;
  }

  int dir_out(int, int) {
    Dvle& d = dvle();
    NEXT_ARG_SPC(outName);
    NEXT_ARG_SPC(outType);
    char* outDestRegName = nextArgSpc();
    ENSURE_NO_MORE_ARGS();

    int oid = -1;
    int sw = kDefaultOpsrc;
    if (outName[0] == '-' && !outName[1]) outName = nullptr;
    else if (!validateIdentifier(outName)) return throwError("invalid identifier: %s\n", outName);

    if (outDestRegName) {
      ARG_TO_REG(outDestReg, outDestRegName);
      if (outDestReg < 0x00 || outDestReg >= 0x10) return throwError("invalid output register: %s\n", outDestRegName);
      oid = outDestReg;
      sw = outDestRegSw;
    }
    if (oid < 0) {
      char* dotPos = std::strchr(outType, '.');
      if (dotPos) {
        *dotPos++ = 0;
        sw = parseSwizzling(dotPos);
        if (sw < 0) return throwError("invalid output mask: %s\n", dotPos);
      }
    }
    int mask = maskFromSwizzling(sw, false);
    int type = parseOutType(outType);
    if (type < 0) return throwError("invalid output type: %s\n", outType);

    if (oid < 0) oid = d.findFreeOutput();
    else if (d.outputUsedReg & (u32(mask) << (4 * oid))) return throwError("this output collides with another one previously defined\n");

    if (oid < 0 || d.outputCount == kMaxOutput) return throwError("too many outputs\n");
    if (outName && aliases_.find(outName) != aliases_.end()) return duplicateIdentifier(outName);
    if (oid >= 7 && type != OUTTYPE_DUMMY) return throwError("this register (o%d) can only be a dummy output\n", oid);

    d.outputTable[d.outputCount++] = outputMake(type, oid, mask);
    d.outputMask |= u16(bit(oid));
    d.outputUsedReg |= u32(mask) << (4 * oid);
    if (outName) aliases_.insert(std::make_pair(std::string(outName), oid | (kDefaultOpsrc << 8)));
    return 0;
  }

  int dir_entry(int, int) {
    Dvle& d = dvle();
    NEXT_ARG_SPC(entrypoint);
    ENSURE_NO_MORE_ARGS();
    if (!validateIdentifier(entrypoint)) return throwError("invalid identifier: %s\n", entrypoint);
    d.entrypoint = entrypoint;
    return 0;
  }

  int dir_unsupported(int, int) { return throwError("this directive is not supported by the vertex shader assembler (geometry shaders, .nodvle)\n"); }

  // ------------------------------------------------------------------ driver
  int processCommand(const char* cmd);
  int fixupLabelRelocations();
  int assembleString(char* str);
  int relocateProduct();
  void writeShbin(PicaAsmResult& R);
};

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// command tables and driver
// ---------------------------------------------------------------------------------------------------------------------------------------------------------
int Assembler::processCommand(const char* cmd) {
  static const Cmd cmdTable[] = {
      {"NOP", &Assembler::cmd_format0, MAESTRO_NOP, -1},
      {"END", &Assembler::cmd_format0, MAESTRO_END, -1},
      {"EMIT", &Assembler::cmd_unsupported, MAESTRO_EMIT, -1},
      {"BREAK", &Assembler::cmd_format0, MAESTRO_BREAK, -1},

      {"ADD", &Assembler::cmd_format1, MAESTRO_ADD, -1},
      {"DP3", &Assembler::cmd_format1, MAESTRO_DP3, -1},
      {"DP4", &Assembler::cmd_format1, MAESTRO_DP4, -1},
      {"DPH", &Assembler::cmd_format1, MAESTRO_DPH, MAESTRO_DPHI},
      {"DPHI", &Assembler::cmd_format1, MAESTRO_DPH, MAESTRO_DPHI},
      {"DST", &Assembler::cmd_format1, MAESTRO_DST, MAESTRO_DSTI},
      {"DSTI", &Assembler::cmd_format1, MAESTRO_DST, MAESTRO_DSTI},
      {"MUL", &Assembler::cmd_format1, MAESTRO_MUL, -1},
      {"SGE", &Assembler::cmd_format1, MAESTRO_SGE, MAESTRO_SGEI},
      {"SGEI", &Assembler::cmd_format1, MAESTRO_SGE, MAESTRO_SGEI},
      {"SLT", &Assembler::cmd_format1, MAESTRO_SLT, MAESTRO_SLTI},
      {"SLTI", &Assembler::cmd_format1, MAESTRO_SLT, MAESTRO_SLTI},
      {"MAX", &Assembler::cmd_format1, MAESTRO_MAX, -1},
      {"MIN", &Assembler::cmd_format1, MAESTRO_MIN, -1},

      {"EX2", &Assembler::cmd_format1u, MAESTRO_EX2, -1},
      {"LG2", &Assembler::cmd_format1u, MAESTRO_LG2, -1},
      {"LITP", &Assembler::cmd_format1u, MAESTRO_LITP, -1},
      {"FLR", &Assembler::cmd_format1u, MAESTRO_FLR, -1},
      {"RCP", &Assembler::cmd_format1u, MAESTRO_RCP, -1},
      {"RSQ", &Assembler::cmd_format1u, MAESTRO_RSQ, -1},
      {"MOV", &Assembler::cmd_format1u, MAESTRO_MOV, -1},

      {"MOVA", &Assembler::cmd_formatmova, MAESTRO_MOVA, -1},
      {"CMP", &Assembler::cmd_format1c, MAESTRO_CMP, -1},
      {"CALL", &Assembler::cmd_formatcall, MAESTRO_CALL, -1},
      {"FOR", &Assembler::cmd_formatfor, MAESTRO_FOR, -1},

      {"BREAKC", &Assembler::cmd_format2, MAESTRO_BREAKC, -1},
      {"CALLC", &Assembler::cmd_format2, MAESTRO_CALLC, -1},
      {"IFC", &Assembler::cmd_format2, MAESTRO_IFC, -1},
      {"JMPC", &Assembler::cmd_format2, MAESTRO_JMPC, -1},

      {"CALLU", &Assembler::cmd_format3, MAESTRO_CALLU, -1},
      {"IFU", &Assembler::cmd_format3, MAESTRO_IFU, -1},
      {"JMPU", &Assembler::cmd_format3, MAESTRO_JMPU, -1},

      {"MAD", &Assembler::cmd_format5, MAESTRO_MAD, MAESTRO_MADI},
      {"MADI", &Assembler::cmd_format5, MAESTRO_MAD, MAESTRO_MADI},

      {"SETEMIT", &Assembler::cmd_unsupported, MAESTRO_SETEMIT, -1},
      {nullptr, nullptr, 0, 0},
  };
  static const Cmd dirTable[] = {
      {"proc", &Assembler::dir_proc, 0, 0},
      {"else", &Assembler::dir_else, 0, 0},
      {"end", &Assembler::dir_end, 0, 0},
      {"alias", &Assembler::dir_alias, 0, 0},
      {"fvec", &Assembler::dir_uniform, UTYPE_FVEC, 0},
      {"ivec", &Assembler::dir_uniform, UTYPE_IVEC, 0},
      {"bool", &Assembler::dir_uniform, UTYPE_BOOL, 0},
      {"constf", &Assembler::dir_const, UTYPE_FVEC, 0},
      {"consti", &Assembler::dir_const, UTYPE_IVEC, 0},
      {"constfa", &Assembler::dir_constfa, 0, 0},
      {"in", &Assembler::dir_in, 0, 0},
      {"out", &Assembler::dir_out, 0, 0},
      {"entry", &Assembler::dir_entry, 0, 0},
      {"nodvle", &Assembler::dir_unsupported, 0, 0},
      {"gsh", &Assembler::dir_unsupported, 0, 0},
      {"setf", &Assembler::dir_setfi, UTYPE_FVEC, 0},
      {"seti", &Assembler::dir_setfi, UTYPE_IVEC, 0},
      {"setb", &Assembler::dir_setb, 0, 0},
      {nullptr, nullptr, 0, 0},
  };

  const Cmd* table = cmdTable;
  if (*cmd == '.') {
    cmd++;
    table = dirTable;
  } else if (!stackPos_) {
    return throwError("instruction outside block\n");
  } else {
    lastWasEnd_ = false;
    dvle();
    if (buf_.size() > size_t(kMaxVshSize))
      return throwError("instruction outside vertex shader code memory (max %d instructions, currently %d)\n", kMaxVshSize, int(buf_.size()));
  }
  for (int i = 0; table[i].name; i++)
    if (stricmpEq(table[i].name, cmd)) return (this->*table[i].func)(table[i].opcode, table[i].opcodei);
  return throwError("invalid instruction: %s\n", cmd);
}

int Assembler::fixupLabelRelocations() {
  for (const Relocation& r : labelRelocs_) {
    u32& inst = buf_[r.first];
    auto lbl = labels_.find(r.second);
    if (lbl == labels_.end()) return throwError("label '%s' is undefined\n", r.second.c_str());
    u32 dst = u32(lbl->second);
    inst &= ~(0xFFFu << 10);
    inst |= dst << 10;
  }
  return 0;
}

int Assembler::assembleString(char* str) {
  curLine_ = 1;
  int nextLineIncr = 0;
  char* nextStr = nullptr;
  for (; str; str = nextStr, curLine_ += nextLineIncr) {
    size_t len = std::strcspn(str, "\n");
    int linedelim = str[len];
    str[len] = 0;
    nextStr = linedelim ? (str + len + 1) : nullptr;
    nextLineIncr = linedelim == '\n' ? 1 : 0;

    char* line = trimWhitespace(removeComment(str));
    for (;;) {
      char* colonPos = std::strchr(line, ':');
      if (!colonPos) break;
      *colonPos = 0;
      char* labelName = line;
      line = trimWhitespace(colonPos + 1);
      if (!validateIdentifier(labelName)) return throwError("invalid label name: %s\n", labelName);
      if (!labels_.insert(std::make_pair(std::string(labelName), buf_.size())).second) return throwError("duplicate label: %s\n", labelName);
    }
    if (!*line) continue;

    if (*line == '#') {   // preprocessor line marker: # <line> "<file>"
      line = trimWhitespace(line + 1);
      nextLineIncr = 0;
      size_t pos = std::strcspn(line, " \t");
      line[pos] = 0;
      curLine_ = std::atoi(line);
      line = trimWhitespace(line + pos + 1);
      if (*line == '"') {
        line++;
        line[std::strlen(line) - 1] = 0;
      }
      curFile_ = line;
      continue;
    }

    char* tok = mystrtokSpc(line);
    SAFE(processCommand(tok));
  }
  if (stackPos_) return throwError("unclosed block(s)\n");
  SAFE(fixupLabelRelocations());
  return 0;
}

int Assembler::relocateProduct() {
  for (const Relocation& r : procRelocs_) {
    u32& inst = buf_[r.first];
    auto proc = procTable_.find(r.second);
    if (proc == procTable_.end()) return throwError("procedure '%s' is undefined\n", r.second.c_str());
    u32 dst = u32(proc->second.first);
    u32 num = u32(proc->second.second);
    inst &= ~0x3FFFFFu;
    inst |= num | (dst << 10);
  }
  if (!haveDvle_) return throwError("no DVLEs can be generated from the given input file(s)\n");
  curLine_ = 1;
  auto mainIt = procTable_.find(dvle_.entrypoint);
  if (mainIt == procTable_.end()) return throwError("entrypoint '%s' is undefined\n", dvle_.entrypoint.c_str());
  dvle_.entryStart = mainIt->second.first;
  dvle_.entryEnd = dvle_.entryStart + mainIt->second.second;
  return 0;
}

namespace {
struct Writer {
  std::vector<uint8_t>& v;
  void b(u32 x) { v.push_back(uint8_t(x)); }
  void h(u32 x) { b(x); b(x >> 8); }
  void w(u32 x) { h(x); h(x >> 16); }
  void d(u64 x) { w(u32(x)); w(u32(x >> 32)); }
  void raw(const char* s, size_t n) { v.insert(v.end(), s, s + n); }
};
}  // namespace

void Assembler::writeShbin(PicaAsmResult& R) {
  std::vector<uint8_t>& out = R.shbin;
  Writer f{out};
  Dvle& dv = dvle_;

  u32 progSize = u32(buf_.size());
  u32 dvlpSize = 10 * 4 + progSize * 4 + u32(opdescCount_) * 8;

  f.w(0x424C5644);   // DVLB
  f.w(1);            // number of DVLEs
  u32 curOff = 2 * 4 + 1 * 4 + dvlpSize;
  f.w(curOff);

  // DVLP
  f.w(0x504C5644);
  f.w(0);
  f.w(10 * 4);
  f.w(progSize);
  f.w(10 * 4 + progSize * 4);
  f.w(u32(opdescCount_));
  f.w(dvlpSize);
  f.w(0);
  f.w(0);
  f.w(0);
  for (u32 w : buf_) f.w(w);
  for (int i = 0; i < opdescCount_; i++) f.d(u64(int64_t(opdescTable_[i])));

  // DVLE
  std::stable_sort(dv.uniformTable, dv.uniformTable + dv.uniformCount);
  curOff = 16 * 4;
  f.w(0x454C5644);
  f.h(0x1002);
  f.b(0);   // shader type: vertex
  f.b(0);   // merge
  f.w(u32(dv.entryStart));
  f.w(u32(dv.entryEnd));
  f.h(dv.inputMask);
  f.h(dv.outputMask);
  f.b(0);
  f.b(0);
  f.b(0);
  f.b(0);
  f.w(curOff);
  f.w(u32(dv.constantCount));
  curOff += u32(dv.constantCount) * 5 * 4;
  f.w(curOff);
  f.w(0);
  f.w(curOff);
  f.w(u32(dv.outputCount));
  curOff += u32(dv.outputCount) * 8;
  f.w(curOff);
  f.w(u32(dv.uniformCount));
  curOff += u32(dv.uniformCount) * 8;
  f.w(curOff);
  f.w(u32(dv.symbolSize));

  for (int i = 0; i < dv.constantCount; i++) {
    Constant& ct = dv.constantTable[i];
    f.h(u32(ct.type));
    if (ct.type == UTYPE_FVEC) {
      f.h(u32(ct.regId - 0x20));
      for (int j = 0; j < 4; j++) f.w(picaF32toF24(ct.fparam[j]));
    } else if (ct.type == UTYPE_IVEC) {
      f.h(u32(ct.regId - 0x80));
      for (int j = 0; j < 4; j++) f.b(ct.iparam[j]);
    } else if (ct.type == UTYPE_BOOL) {
      f.h(u32(ct.regId - 0x88));
      f.w(ct.bparam ? 1 : 0);
    }
    if (ct.type != UTYPE_FVEC)
      for (int j = 0; j < 3; j++) f.w(0);
  }
  for (int i = 0; i < dv.outputCount; i++) f.d(dv.outputTable[i]);

  size_t sp = 0;
  for (int i = 0; i < dv.uniformCount; i++) {
    Uniform& u = dv.uniformTable[i];
    size_t l = u.name.length() + 1;
    f.w(u32(sp));
    sp += l;
    int pos = u.pos;
    if (pos >= 0x20) pos -= 0x10;
    f.h(u32(pos));
    f.h(u32(pos + u.size - 1));
  }
  for (int i = 0; i < dv.uniformCount; i++) {
    std::string u(dv.uniformTable[i].name);
    std::replace(u.begin(), u.end(), '$', '.');
    f.raw(u.c_str(), u.length() + 1);
  }
  while (out.size() & 3) out.push_back(0);
}

bool Assembler::run(const std::string& source, PicaAsmResult& R) {
  std::vector<char> text(source.begin(), source.end());
  text.push_back(0);
  if (assembleString(text.data()) != 0 || relocateProduct() != 0) {
    R.error = err_;
    R.warnings = warnings_;
    return false;
  }
  writeShbin(R);
  R.warnings = warnings_;
  R.code = buf_;
  for (int i = 0; i < opdescCount_; i++) R.opdescs.push_back(u32(opdescTable_[i]));
  for (int i = 0; i < dvle_.uniformCount; i++) {
    PicaBinUniform u;
    u.name = dvle_.uniformTable[i].name;
    u.pos = dvle_.uniformTable[i].pos;
    u.size = dvle_.uniformTable[i].size;
    u.type = dvle_.uniformTable[i].type;
    R.uniforms.push_back(u);
  }
  for (int i = 0; i < dvle_.constantCount; i++) {
    PicaBinConstant c;
    const Constant& s = dvle_.constantTable[i];
    c.regId = s.regId;
    c.type = s.type;
    for (int j = 0; j < 4; j++) { c.f[j] = s.fparam[j]; c.i[j] = s.iparam[j]; }
    c.b = s.bparam;
    R.constants.push_back(c);
  }
  for (int i = 0; i < dvle_.outputCount; i++) {
    PicaBinOutput o;
    o.type = int(dvle_.outputTable[i] & 0xFFFF);
    o.reg = int((dvle_.outputTable[i] >> 16) & 0xFFFF);
    o.mask = int(dvle_.outputTable[i] >> 32);
    R.outputs.push_back(o);
  }
  R.entryStart = u32(dvle_.entryStart);
  R.entryEnd = u32(dvle_.entryEnd);
  R.inputMask = dvle_.inputMask;
  R.outputMask = dvle_.outputMask;
  R.ok = true;
  return true;
}

}  // namespace

PicaAsmResult assemblePica(const std::string& source, const std::string& filename, bool autoNop) {
  PicaAsmResult R;
  Assembler A(filename, autoNop);
  A.run(source, R);
  return R;
}

}  // namespace t2p
