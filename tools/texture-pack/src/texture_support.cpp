// Exact shared helper bodies; avoids linking the game/renderer into the converter.
#include "gxstate.h"
#include "texcache.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace t2p {
uint64_t hashBytes(const uint8_t* p, size_t n, uint64_t seed) {
  uint64_t h = seed * 0x9E3779B97F4A7C15ULL + 0xD6E8FEB86659FD93ULL + n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 29;
  }
  uint64_t tail = 0;
  for (size_t k = 0; i + k < n; k++) tail |= uint64_t(p[i + k]) << (8 * k);
  h = (h ^ tail) * 0x9E3779B97F4A7C15ULL;
  h ^= h >> 32;
  return h;
}
int GxTexBinding::mipLevels() const {
  if (mipMode == 0 || width <= 0 || height <= 0) return 1;
  int top = 0;
  for (int m = std::max(width, height); m > 1; m >>= 1) top++;
  return std::min(top, int(std::floor(maxLod))) + 1;
}
}
