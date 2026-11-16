#include "picaasm.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace t2p {

float roundF24(float x) {
  uint32_t b;
  std::memcpy(&b, &x, 4);
  uint32_t e = (b >> 23) & 0xFF;
  if (e == 0xFF) return x;
  if (e == 0) { b &= 0x80000000u; std::memcpy(&x, &b, 4); return x; }  // float24 has no float32-denormal range
  uint32_t low = b & 0x7F;
  b &= ~0x7Fu;
  if (low > 0x40 || (low == 0x40 && (b & 0x80))) b += 0x80;  // round to nearest even on the 16-bit mantissa
  std::memcpy(&x, &b, 4);
  return x;
}

namespace {

struct Alias { char file; int reg; bool neg; int swz[4]; };

static std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}
static std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}
static int compOf(char c) {
  switch (std::tolower((unsigned char)c)) {
    case 'x': case 'r': case 's': return 0;
    case 'y': case 'g': case 't': return 1;
    case 'z': case 'b': case 'p': return 2;
    case 'w': case 'a': case 'q': return 3;
  }
  return -1;
}
static bool parseSwz(const std::string& s, int out[4]) {
  if (s.empty() || s.size() > 4) return false;
  int last = 0;
  for (size_t i = 0; i < 4; i++) {
    if (i < s.size()) { last = compOf(s[i]); if (last < 0) return false; }
    out[i] = last;
  }
  return true;
}
static std::vector<std::string> splitArgs(const std::string& s) {
  std::vector<std::string> v;
  std::string cur;
  int depth = 0;
  for (char c : s) {
    if (c == '[' || c == '(') depth++;
    if (c == ']' || c == ')') depth--;
    if (c == ',' && depth == 0) { v.push_back(trim(cur)); cur.clear(); }
    else cur += c;
  }
  if (!trim(cur).empty() || !v.empty()) v.push_back(trim(cur));
  return v;
}

// ---- operand-descriptor bookkeeping, ported from picasso (findOrAddOpdesc / optimizeOpdesc)
#define SWZC(n, v) ((v) << (6 - (n) * 2))
static inline int opsrcMake(int neg, int sw) { return neg | (sw << 1); }
static inline int opdescMake(int out, int s1, int s2, int s3) { return out | (s1 << 4) | (s2 << 13) | (s3 << 22); }
static const int MASK_D123 = opdescMake(0xF, 0x1FF, 0x1FF, 0x1FF);
static const int MASK_D12 = opdescMake(0xF, 0x1FF, 0x1FF, 0);
static const int MASK_D1 = opdescMake(0xF, 0x1FF, 0, 0);
static const int MASK_12 = opdescMake(0, 0x1FF, 0x1FF, 0);

struct OpdescTable {
  int table[512] = {}, masks[512] = {}, count = 0;
  unsigned isMad = 0;
  bool overflow = false;
};

static void optimizeOpdesc(int& mask, const std::string& op, int opdesc) {
  int u1 = 0, u2 = 0, u3 = 0;
  auto elementwise = [&]() {
    for (int i = 0; i < 4; i++)
      if (!(opdesc & (1 << (3 - i)))) u1 |= SWZC(i, 3);
    u2 = u1; u3 = u1;
  };
  if (op == "add" || op == "mul" || op == "sge" || op == "slt" || op == "flr" || op == "max" || op == "min" ||
      op == "mov" || op == "mad") elementwise();
  else if (op == "dp3") { u1 = SWZC(3, 3); u2 = SWZC(3, 3); }
  else if (op == "dph") { u1 = SWZC(3, 3); }
  else if (op == "ex2" || op == "lg2" || op == "rcp" || op == "rsq") u1 = SWZC(1, 3) | SWZC(2, 3) | SWZC(3, 3);
  else if (op == "mova") {
    if (!(opdesc & (1 << (3 - 0)))) u1 |= SWZC(0, 3);
    if (!(opdesc & (1 << (3 - 1)))) u1 |= SWZC(1, 3);
    u1 |= SWZC(2, 3) | SWZC(3, 3);
  }
  mask &= ~opdescMake(0, opsrcMake(0, u1), opsrcMake(0, u2), opsrcMake(0, u3));
}
static int findOrAddOpdesc(OpdescTable& t, const std::string& op, int opdesc, int mask) {
  optimizeOpdesc(mask, op, opdesc);
  for (int i = 0; i < t.count; i++) {
    int minMask = mask & t.masks[i];
    if ((opdesc & minMask) == (t.table[i] & minMask)) {
      t.table[i] = (t.table[i] & ~mask) | (opdesc & mask);
      t.masks[i] |= mask;
      return i;
    }
  }
  if (t.count == 128) { t.overflow = true; return -1; }
  t.table[t.count] = opdesc;
  t.masks[t.count] = mask;
  return t.count++;
}

struct Parser {
  PicaProgram2& P;
  std::string err;
  int line = 0;
  std::map<std::string, Alias> aliases;
  int fvecNext = 0, constNext = 96;
  unsigned outRegMask = 0, outCompUsed[16] = {};
  unsigned inputMask = 0;
  bool inProc = false, sawEnd = false, lastWasMova = false;
  OpdescTable od;

  explicit Parser(PicaProgram2& p) : P(p) {}

  bool fail(const std::string& m) {
    if (err.empty()) err = "line " + std::to_string(line) + ": " + m;
    return false;
  }

  static bool isRegName(const std::string& s) {
    return s.size() >= 2 && std::strchr("ovrcOVRC", s[0]) && std::isdigit((unsigned char)s[1]);
  }

  // Parses "[-]name[idx].swz". allowIdx: relative addressing allowed. Fills op (file/reg/idx/neg/swz).
  bool operand(const std::string& text, PicaOperand& op, bool allowIdx) {
    std::string s = trim(text);
    op = PicaOperand();
    if (s.empty()) return fail("empty operand");
    bool neg = false;
    if (s[0] == '-') { neg = true; s = trim(s.substr(1)); }
    std::string name, inner, rest;
    size_t lb = s.find('[');
    if (lb != std::string::npos) {
      size_t rb = s.find(']', lb);
      if (rb == std::string::npos) return fail("missing closing bracket: " + text);
      name = trim(s.substr(0, lb));
      inner = trim(s.substr(lb + 1, rb - lb - 1));
      rest = s.substr(rb + 1);
    } else {
      size_t dot = s.find('.');
      name = trim(dot == std::string::npos ? s : s.substr(0, dot));
      rest = dot == std::string::npos ? "" : s.substr(dot);
    }
    int swz[4] = {0, 1, 2, 3};
    if (!rest.empty()) {
      size_t dot = rest.find('.');
      if (dot == std::string::npos || dot != 0) return fail("garbage after operand: " + text);
      if (!parseSwz(rest.substr(1), swz)) return fail("invalid swizzling mask: " + text);
    }
    int constOff = 0, idx = 0;
    if (!inner.empty()) {
      std::string idxName = inner, offs;
      size_t plus = inner.find('+');
      if (plus != std::string::npos) { idxName = trim(inner.substr(0, plus)); offs = trim(inner.substr(plus + 1)); }
      std::string ln = lower(idxName);
      int t = (ln == "a0" || ln == "a0.x") ? 1 : (ln == "a1" || ln == "a0.y") ? 2 : (ln == "a2" || ln == "lcnt" || ln == "al") ? 3 : 0;
      if (t) {
        if (!allowIdx) return fail("index register not allowed here: " + text);
        idx = t;
        if (!offs.empty()) { constOff = std::atoi(offs.c_str()); if (constOff < 0) return fail("invalid register offset: " + text); }
      } else {
        if (plus != std::string::npos) return fail("invalid index register: " + idxName);
        constOff = std::atoi(inner.c_str());
        if (constOff < 0) return fail("invalid register offset: " + text);
      }
    }
    Alias base;
    auto it = aliases.find(name);
    if (it != aliases.end()) base = it->second;
    else {
      if (!isRegName(name)) return fail("invalid register: " + name);
      char f = char(std::tolower((unsigned char)name[0]));
      int n = std::atoi(name.c_str() + 1);
      base = Alias{f, n, false, {0, 1, 2, 3}};
    }
    op.file = base.file;
    op.reg = base.reg + constOff;
    op.idx = idx;
    op.neg = neg ^ base.neg;
    for (int j = 0; j < 4; j++) op.swz[j] = base.swz[swz[j]];
    if (idx && op.file != 'c') return fail("index register not allowed with this kind of register: " + text);
    int lim = op.file == 'c' ? 96 : 16;
    if (op.file == 'v') lim = 15;  // picasso rejects v15 in instructions (off-by-one in its bounds check)
    if (op.reg < 0 || op.reg >= lim) return fail("register out of range: " + text);
    return true;
  }

  static int srcField(const PicaOperand& o) {
    int sw = 0;
    for (int j = 0; j < 4; j++) sw |= o.swz[j] << (6 - 2 * j);
    return opsrcMake(o.neg ? 1 : 0, sw);
  }
  static bool wide(const PicaOperand& o) { return o.file == 'c' || o.idx != 0; }
  static bool narrow(const PicaOperand& o) { return (o.file == 'v' || o.file == 'r') && o.idx == 0; }

  bool instruction(const std::string& opname, const std::vector<std::string>& args) {
    PicaIns I;
    I.op = opname;
    I.line = line;
    const std::string& o = opname;
    if (o == "nop" || o == "end") {
      if (!args.empty()) return fail("too many parameters");
      if (o == "end") sawEnd = true;
      P.ins.push_back(I);
      lastWasMova = false;
      return true;
    }
    int nsrc = 0;
    bool inv = false;
    if (o == "add" || o == "dp3" || o == "dp4" || o == "mul" || o == "max" || o == "min" || o == "dst") nsrc = 2;
    else if (o == "dph" || o == "sge" || o == "slt") { nsrc = 2; inv = true; }
    else if (o == "ex2" || o == "lg2" || o == "flr" || o == "rcp" || o == "rsq" || o == "mov" || o == "mova") nsrc = 1;
    else if (o == "mad") { nsrc = 3; inv = true; }
    else return fail("unsupported instruction in this front end: " + o);
    if (int(args.size()) != nsrc + 1) return fail(std::string(args.size() < size_t(nsrc + 1) ? "missing" : "too many") + " parameters for " + o);
    I.nsrc = nsrc;

    int maskBits = 0;  // picasso opdesc order: x = bit3
    if (o == "mova") {
      std::string d = lower(args[0]);
      bool x = (d == "a0" || d == "a0.x" || d == "a01" || d == "a0.xy");
      bool y = (d == "a1" || d == "a0.y" || d == "a01" || d == "a0.xy");
      if (!x && !y) return fail("invalid destination register for mova: " + args[0]);
      I.dst.file = 'a';
      I.dstMask[0] = x; I.dstMask[1] = y; I.dstMask[2] = I.dstMask[3] = false;
      maskBits = (x ? 8 : 0) | (y ? 4 : 0);
      if (lastWasMova) return fail("two consecutive mova instructions freeze the PICA200");
    } else {
      if (!operand(args[0], I.dst, false)) return false;
      if (I.dst.file != 'r' && I.dst.file != 'o') return fail("invalid destination register (only r and o are writable): " + args[0]);
      if (I.dst.neg) return fail("negated destination: " + args[0]);
      bool used[4] = {false, false, false, false};
      for (int j = 0; j < 4; j++) used[I.dst.swz[j]] = true;
      for (int c = 0; c < 4; c++) { I.dstMask[c] = used[c]; if (used[c]) maskBits |= 1 << (3 - c); }
    }
    for (int i = 0; i < nsrc; i++) {
      if (!operand(args[1 + i], I.src[i], true)) return false;
      if (I.src[i].file == 'o') return fail("output register used as a source (picasso would read input register of the same number): " + args[1 + i]);
    }
    // ---- operand class rules (picasso format1 / format1u / format5)
    const PicaOperand* s = I.src;
    if (nsrc == 1) {
      // wide
    } else if (nsrc == 2) {
      bool inverted = inv && narrow(s[0]) && wide(s[1]);
      if (inverted) { /* narrow, wide */ }
      else {
        if (!narrow(s[1])) return fail(std::string("source2 of ") + o + " must be a v/r register (uniforms only fit source1): " + args[2]);
      }
    } else {  // mad
      if (!narrow(s[0])) return fail("source1 of mad must be a v/r register: " + args[1]);
      bool inverted = narrow(s[1]) && (wide(s[2]));
      if (inverted) { /* narrow, narrow, wide */ }
      else {
        if (!narrow(s[2])) return fail("mad: source3 must be a v/r register unless source2 is");
      }
    }
    // ---- one distinct input register
    int vfirst = -1;
    for (int i = 0; i < nsrc; i++) if (s[i].file == 'v') {
      if (vfirst >= 0 && vfirst != s[i].reg) return fail("source operands must be different input registers (v0..v15): use a single v register per instruction");
      vfirst = s[i].reg;
    }
    // ---- operand descriptor
    int f1 = nsrc >= 1 ? srcField(s[0]) : 0, f2 = nsrc >= 2 ? srcField(s[1]) : 0, f3 = nsrc >= 3 ? srcField(s[2]) : 0;
    int desc = opdescMake(maskBits, f1, f2, f3);
    int dmask = nsrc == 3 ? MASK_D123 : nsrc == 2 ? MASK_D12 : MASK_D1;
    int slot = findOrAddOpdesc(od, o, desc, dmask);
    if (slot < 0) return fail("too many operand descriptors (limit is 128): the shader uses too many distinct swizzle/mask/negate combinations");
    if (o == "mad") {
      if (slot >= 32) {
        int which = 0;
        while (which < 32 && (od.isMad & (1u << which))) which++;
        if (which == 32) return fail("opdesc allocation error: more than 32 distinct mad operand descriptors");
        std::swap(od.table[which], od.table[slot]);
        std::swap(od.masks[which], od.masks[slot]);
        slot = which;
      }
      od.isMad |= 1u << slot;
    }
    lastWasMova = (o == "mova");
    P.ins.push_back(I);
    if (P.ins.size() > 512) return fail("instruction outside vertex shader code memory (max 512 instructions)");
    return true;
  }

  bool directive(const std::string& d, const std::string& rest) {
    if (d == ".proc") {
      if (inProc) return fail("too many nested blocks");
      inProc = true;
      return true;
    }
    if (d == ".end") {
      if (!inProc) return fail(".end with unmatched block");
      inProc = false;
      return true;
    }
    if (d == ".entry" || d == ".nodvle") return true;
    if (d == ".fvec") {
      for (const std::string& a : splitArgs(rest)) {
        std::string nm = a;
        int sz = 1;
        size_t lb = a.find('[');
        if (lb != std::string::npos) {
          size_t rb = a.find(']', lb);
          if (rb == std::string::npos) return fail("missing closing bracket: " + a);
          nm = trim(a.substr(0, lb));
          sz = std::atoi(trim(a.substr(lb + 1, rb - lb - 1)).c_str());
          if (sz < 1) return fail("invalid uniform size: " + a);
        }
        if (nm.empty() || isRegName(nm)) return fail("invalid uniform name: " + nm);
        if (aliases.count(nm)) return fail("identifier already used: " + nm);
        if (fvecNext + sz > constNext) return fail("not enough uniform space: " + nm + "[" + std::to_string(sz) + "]");
        aliases[nm] = Alias{'c', fvecNext, false, {0, 1, 2, 3}};
        if (nm[0] != '_') P.uniform[nm] = {fvecNext, sz};
        fvecNext += sz;
        P.fvecRegs = fvecNext;
      }
      return true;
    }
    if (d == ".constf") {
      size_t lp = rest.find('('), rp = rest.rfind(')');
      if (lp == std::string::npos || rp == std::string::npos || rp < lp) return fail("invalid syntax");
      std::string nm = trim(rest.substr(0, lp));
      std::vector<std::string> v = splitArgs(rest.substr(lp + 1, rp - lp - 1));
      if (v.size() != 4) return fail("constf needs 4 components");
      if (nm.empty() || aliases.count(nm)) return fail("identifier already used: " + nm);
      if (constNext - 1 < fvecNext) return fail("not enough space for local constant '" + nm + "'");
      constNext--;
      PVec c;
      for (int j = 0; j < 4; j++) c.v[j] = float(std::atof(v[j].c_str()));
      P.constants[constNext] = c;
      P.constRegs = 96 - constNext;
      aliases[nm] = Alias{'c', constNext, false, {0, 1, 2, 3}};
      return true;
    }
    if (d == ".alias") {
      std::vector<std::string> parts;
      std::string cur;
      for (char ch : rest) {
        if (std::isspace((unsigned char)ch)) { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur += ch;
      }
      if (!cur.empty()) parts.push_back(cur);
      if (parts.size() != 2) return fail("alias needs a name and a register");
      if (isRegName(parts[0])) return fail("cannot redefine register");
      if (aliases.count(parts[0])) return fail("identifier already used: " + parts[0]);
      PicaOperand op;
      if (!operand(parts[1], op, false)) return false;
      aliases[parts[0]] = Alias{op.file, op.reg, op.neg, {op.swz[0], op.swz[1], op.swz[2], op.swz[3]}};
      return true;
    }
    if (d == ".in") {
      std::vector<std::string> parts;
      std::string cur;
      for (char ch : rest) {
        if (std::isspace((unsigned char)ch)) { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur += ch;
      }
      if (!cur.empty()) parts.push_back(cur);
      if (parts.empty() || parts.size() > 2) return fail("bad .in");
      if (aliases.count(parts[0])) return fail("identifier already used: " + parts[0]);
      int reg = -1;
      if (parts.size() == 2) {
        if (lower(parts[1])[0] != 'v' || !std::isdigit((unsigned char)parts[1][1])) return fail("invalid input register: " + parts[1]);
        reg = std::atoi(parts[1].c_str() + 1);
        if (reg < 0 || reg >= 16) return fail("invalid input register: " + parts[1]);
      } else {
        for (int i = 0; i < 16; i++) if (!(inputMask & (1u << i))) { reg = i; break; }
        if (reg < 0) return fail("too many inputs");
      }
      inputMask |= 1u << reg;
      P.inputs[parts[0]] = reg;
      aliases[parts[0]] = Alias{'v', reg, false, {0, 1, 2, 3}};
      return true;
    }
    if (d == ".out") {
      std::vector<std::string> parts;
      std::string cur;
      for (char ch : rest) {
        if (std::isspace((unsigned char)ch)) { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur += ch;
      }
      if (!cur.empty()) parts.push_back(cur);
      if (parts.size() < 2 || parts.size() > 3) return fail("bad .out");
      std::string nm = parts[0] == "-" ? "" : parts[0];
      std::string prop = parts[1];
      int sw[4] = {0, 1, 2, 3};
      int oid = -1;
      if (parts.size() == 3) {
        PicaOperand op;
        if (!operand(parts[2], op, false)) return false;
        if (op.file != 'o') return fail("invalid output register: " + parts[2]);
        oid = op.reg;
        for (int j = 0; j < 4; j++) sw[j] = op.swz[j];
      } else {
        size_t dot = prop.find('.');
        if (dot != std::string::npos) {
          if (!parseSwz(prop.substr(dot + 1), sw)) return fail("invalid output mask: " + prop);
          prop = prop.substr(0, dot);
        }
      }
      int mask = 0;
      for (int j = 0; j < 4; j++) mask |= 1 << sw[j];
      std::string p = lower(prop);
      static const std::pair<const char*, const char*> kProps[] = {
          {"pos", "position"}, {"position", "position"}, {"nquat", "normalquat"}, {"normalquat", "normalquat"},
          {"clr", "color"}, {"color", "color"}, {"tcoord0", "texcoord0"}, {"texcoord0", "texcoord0"},
          {"tcoord0w", "texcoord0w"}, {"texcoord0w", "texcoord0w"}, {"tcoord1", "texcoord1"}, {"texcoord1", "texcoord1"},
          {"tcoord2", "texcoord2"}, {"texcoord2", "texcoord2"}, {"view", "view"}, {"dummy", "dummy"}};
      std::string canon;
      for (auto& kp : kProps) if (p == kp.first) canon = kp.second;
      if (canon.empty()) return fail("invalid output type: " + prop);
      if (oid < 0) {
        for (int i = 0; i < 16; i++) if (!(outRegMask & (1u << i))) { oid = i; break; }
        if (oid < 0) return fail("too many outputs");
      } else if (outCompUsed[oid] & unsigned(mask)) return fail("this output collides with another one previously defined");
      if (oid >= 7 && canon != "dummy") return fail("this register (o" + std::to_string(oid) + ") can only be a dummy output");
      outRegMask |= 1u << oid;
      outCompUsed[oid] |= unsigned(mask);
      P.outputs.push_back({canon, oid, mask});
      if (!nm.empty()) {
        if (aliases.count(nm)) return fail("identifier already used: " + nm);
        aliases[nm] = Alias{'o', oid, false, {0, 1, 2, 3}};
      }
      return true;
    }
    return fail("unsupported directive in this front end: " + d);
  }

  bool run(const std::string& text) {
    size_t pos = 0;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      std::string ln = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
      line++;
      size_t sc = ln.find(';');
      if (sc != std::string::npos) ln.resize(sc);
      ln = trim(ln);
      if (ln.empty()) continue;
      size_t sp = 0;
      while (sp < ln.size() && !std::isspace((unsigned char)ln[sp])) sp++;
      std::string head = lower(ln.substr(0, sp)), rest = trim(ln.substr(sp));
      if (head[0] == '.') { if (!directive(head, rest)) return false; continue; }
      if (head.back() == ':') return fail("labels are not supported by this front end");
      if (!inProc) return fail("instruction outside block");
      if (!instruction(head, splitArgs(rest))) return false;
    }
    if (inProc) return fail("unclosed block(s)");
    if (!sawEnd) return fail("no 'end' instruction");
    P.opdescs = od.count;
    int mads = 0;
    for (int i = 0; i < 32; i++) if (od.isMad & (1u << i)) mads++;
    P.madOpdescs = mads;
    return true;
  }
};

static inline float pmul(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }

}  // namespace

bool parsePica(const std::string& text, PicaProgram2& out, std::string& err) {
  out = PicaProgram2();
  Parser p(out);
  bool ok = p.run(text);
  err = p.err;
  return ok;
}

bool runPica(const PicaProgram2& P, const std::map<std::string, std::vector<PVec>>& uniforms, const PVec in[16],
             std::map<std::string, PVec>& out, std::string& err, bool f24) {
  PVec c[96], r[16], o[16], v[16];
  for (int i = 0; i < 16; i++) v[i] = in[i];
  for (auto& kv : P.constants) c[kv.first] = kv.second;
  for (auto& kv : uniforms) {
    auto it = P.uniform.find(kv.first);
    if (it == P.uniform.end()) { err = "unknown uniform " + kv.first; return false; }
    for (size_t i = 0; i < kv.second.size() && int(i) < it->second.second; i++) c[it->second.first + int(i)] = kv.second[i];
  }
  int a0x = 0, a0y = 0;
  auto R = [&](float x) { return f24 ? roundF24(x) : x; };
  auto fetch = [&](const PicaOperand& op, PVec& dst) -> bool {
    const PVec* src = nullptr;
    switch (op.file) {
      case 'v': src = &v[op.reg]; break;
      case 'r': src = &r[op.reg]; break;
      case 'c': {
        int ri = op.reg + (op.idx == 1 ? a0x : op.idx == 2 ? a0y : 0);
        if (op.idx == 3) { err = "aL relative addressing is not supported by this interpreter"; return false; }
        if (ri < 0 || ri >= 96) { err = "relative address out of range: c" + std::to_string(ri); return false; }
        src = &c[ri];
        break;
      }
      default: err = "bad source file"; return false;
    }
    for (int j = 0; j < 4; j++) { float x = src->v[op.swz[j]]; dst.v[j] = op.neg ? -x : x; }
    return true;
  };
  for (const PicaIns& I : P.ins) {
    if (I.op == "end") break;
    if (I.op == "nop") continue;
    PVec a, b, d, e;
    if (I.nsrc >= 1 && !fetch(I.src[0], a)) return false;
    if (I.nsrc >= 2 && !fetch(I.src[1], b)) return false;
    if (I.nsrc >= 3 && !fetch(I.src[2], e)) return false;
    const std::string& op = I.op;
    if (op == "mova") {
      if (I.dstMask[0]) a0x = int(a.v[0]);
      if (I.dstMask[1]) a0y = int(a.v[1]);
      continue;
    }
    if (op == "add") for (int j = 0; j < 4; j++) d.v[j] = a.v[j] + b.v[j];
    else if (op == "mul") for (int j = 0; j < 4; j++) d.v[j] = pmul(a.v[j], b.v[j]);
    else if (op == "max") for (int j = 0; j < 4; j++) d.v[j] = a.v[j] > b.v[j] ? a.v[j] : b.v[j];
    else if (op == "min") for (int j = 0; j < 4; j++) d.v[j] = a.v[j] < b.v[j] ? a.v[j] : b.v[j];
    else if (op == "sge") for (int j = 0; j < 4; j++) d.v[j] = a.v[j] >= b.v[j] ? 1.0f : 0.0f;
    else if (op == "slt") for (int j = 0; j < 4; j++) d.v[j] = a.v[j] < b.v[j] ? 1.0f : 0.0f;
    else if (op == "flr") for (int j = 0; j < 4; j++) d.v[j] = std::floor(a.v[j]);
    else if (op == "mov") d = a;
    else if (op == "mad") for (int j = 0; j < 4; j++) d.v[j] = pmul(a.v[j], b.v[j]) + e.v[j];
    else if (op == "dp3" || op == "dp4" || op == "dph") {
      float s = pmul(a.v[0], b.v[0]) + pmul(a.v[1], b.v[1]) + pmul(a.v[2], b.v[2]);
      if (op == "dp4") s += pmul(a.v[3], b.v[3]);
      else if (op == "dph") s += b.v[3];
      for (int j = 0; j < 4; j++) d.v[j] = s;
    } else if (op == "dst") {
      d.v[0] = 1.0f; d.v[1] = pmul(a.v[1], b.v[1]); d.v[2] = a.v[2]; d.v[3] = b.v[3];
    } else if (op == "rcp") { float s = 1.0f / a.v[0]; for (int j = 0; j < 4; j++) d.v[j] = s; }
    else if (op == "rsq") { float s = 1.0f / std::sqrt(a.v[0]); for (int j = 0; j < 4; j++) d.v[j] = s; }
    else if (op == "ex2") { float s = std::exp2(a.v[0]); for (int j = 0; j < 4; j++) d.v[j] = s; }
    else if (op == "lg2") { float s = std::log2(a.v[0]); for (int j = 0; j < 4; j++) d.v[j] = s; }
    else { err = "interpreter lacks " + op; return false; }
    PVec* dst = I.dst.file == 'o' ? &o[I.dst.reg] : &r[I.dst.reg];
    for (int j = 0; j < 4; j++) if (I.dstMask[j]) dst->v[j] = R(d.v[j]);
  }
  out.clear();
  for (const PicaOutDecl& od : P.outputs) {
    if (od.prop == "dummy" || od.prop == "view" || od.prop == "normalquat") continue;
    PVec& dst = out[od.prop];
    int k = 0;
    for (int comp = 0; comp < 4; comp++) {
      if (!(od.mask & (1 << comp))) continue;
      if (k < 4) dst.v[k] = o[od.reg].v[comp];
      k++;
    }
  }
  return true;
}

}  // namespace t2p
