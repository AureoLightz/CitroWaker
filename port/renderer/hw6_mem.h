// Guest memory image for hwcheck6: one flat block covering every texture the scenes read (built from hw6_mem.bin).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "texcache.h"

namespace hw6 {

struct FlatMem : t2p::GuestMem {
  uint32_t base = 0;
  std::vector<uint8_t> bytes;
  const uint8_t* read(uint32_t addr, size_t size) const override {
    if (addr < base || size_t(addr - base) + size > bytes.size()) return nullptr;
    return &bytes[addr - base];
  }
  // blocks: u32 count, then (u32 addr, u32 len, bytes) * count, little endian
  bool load(const uint8_t* d, size_t n) {
    if (n < 4) return false;
    uint32_t cnt; std::memcpy(&cnt, d, 4);
    size_t p = 4;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    struct B { uint32_t a, l; size_t at; };
    std::vector<B> bl;
    for (uint32_t i = 0; i < cnt; i++) {
      if (p + 8 > n) return false;
      B b; std::memcpy(&b.a, d + p, 4); std::memcpy(&b.l, d + p + 4, 4); b.at = p + 8; p += 8;
      if (p + b.l > n) return false;
      p += b.l;
      lo = b.a < lo ? b.a : lo; hi = b.a + b.l > hi ? b.a + b.l : hi;
      bl.push_back(b);
    }
    base = lo; bytes.assign(hi - lo, 0);
    for (const B& b : bl) std::memcpy(&bytes[b.a - lo], d + b.at, b.l);
    return true;
  }
};

}  // namespace hw6
