// Converter-only hash declaration; no runtime cache is linked.
#pragma once
#include <cstddef>
#include <cstdint>
namespace t2p {uint64_t hashBytes(const uint8_t*, size_t, uint64_t seed=0);}
