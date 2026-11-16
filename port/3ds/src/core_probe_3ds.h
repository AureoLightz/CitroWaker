// (Claude v94) "[core-probe]": the same fixed workload timed on each CPU core, at boot and during gameplay.
// Question it answers: does a core we use (FIFO worker on core 3, renderer on core 2) give our threads its full speed on the console,
// or does the system take time on it? In Azahar every core runs at full speed, so only console numbers mean anything.
// Workloads: "alu" = 2M dependent integer ops (cache-resident), "mem" = 1M dependent loads over 4 MB (L2 misses, RAM latency).
// Each core runs it 3 times; min and max wall time are logged. A high priority (0x18; core 1: 0x30, the most it accepts) keeps our own
// threads off the core while it runs, so a slow result means time taken by something outside the game.
#pragma once
#include <3ds.h>
#include <cstdint>
#include <cstdlib>

namespace coreprobe {
struct Result { uint64_t alu[3], mem[3]; bool ran; };
struct Job { uint32_t* buf; Result r; };

inline uint32_t aluWork() {
  uint32_t x = 0x12345678u, acc = 0;
  for (int i = 0; i < 2000000; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; acc += x * 2654435761u; }
  return acc;
}
inline uint32_t memWork(const uint32_t* buf) {   // buf[i] holds the next index: one dependent load per step
  uint32_t i = 0, acc = 0;
  for (int k = 0; k < 1000000; k++) { i = buf[i]; acc += i; }
  return acc;
}
inline void threadMain(void* arg) {
  Job* j = static_cast<Job*>(arg);
  volatile uint32_t sink = 0;
  for (int rep = 0; rep < 3; rep++) {
    uint64_t t = svcGetSystemTick(); sink = sink + aluWork(); j->r.alu[rep] = svcGetSystemTick() - t;
    t = svcGetSystemTick(); sink = sink + memWork(j->buf); j->r.mem[rep] = svcGetSystemTick() - t;
  }
  j->r.ran = true;
}

inline void run(const char* when) {
  constexpr uint32_t kWords = 1u << 20;   // 4 MB
  uint32_t* buf = static_cast<uint32_t*>(std::malloc(kWords * 4));
  if (!buf) { n3dsLog("[core-probe] %s: no memory\n", when); return; }
  // one cycle through all words in a scattered order (LCG permutation step, odd multiplier, power-of-two size)
  for (uint32_t i = 0; i < kWords; i++) buf[i] = (i * 2654435761u + 0x9E3779B9u) & (kWords - 1);
  { uint32_t cur = 0; for (uint32_t k = 0; k < kWords; k++) { const uint32_t nxt = (cur * 1664525u + 1013904223u) & (kWords - 1); buf[cur] = nxt; cur = nxt; } }
  const double k = 1000.0 / SYSCLOCK_ARM11;
  for (int core = 0; core < 4; core++) {
    Job j{}; j.buf = buf;
    Thread t = threadCreate(threadMain, &j, 0x2000, core == 1 ? 0x30 : 0x18, core, false);
    if (!t) { n3dsLog("[core-probe] %s core %d: refused\n", when, core); continue; }
    threadJoin(t, U64_MAX); threadFree(t);
    uint64_t amin = ~0ull, amax = 0, mmin = ~0ull, mmax = 0;
    for (int r = 0; r < 3; r++) {
      if (j.r.alu[r] < amin) amin = j.r.alu[r]; if (j.r.alu[r] > amax) amax = j.r.alu[r];
      if (j.r.mem[r] < mmin) mmin = j.r.mem[r]; if (j.r.mem[r] > mmax) mmax = j.r.mem[r];
    }
    n3dsLog("[core-probe] %s core %d: alu min %.2f max %.2f ms, mem min %.2f max %.2f ms\n", when, core, amin * k, amax * k, mmin * k, mmax * k);
  }
  std::free(buf);
}
}  // namespace coreprobe
