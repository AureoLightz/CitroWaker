// Main game-thread wall-clock scopes. Disabled unless N3DS_PROF_GAME=1.
#include <3ds.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "n3ds.h"
#include "prof_game.h"

// (Claude v114) painter groups: the game's PC_GPU_GROUP markers (m_Do_graphic.cpp) split the painter into buckets; each group runs until the next
// one (or the painter's end). Logged with the 60-frame window as [paint-prof].
namespace { struct PaintGroup { const char* name; uint64_t ticks; uint32_t calls; }; PaintGroup paintGroups[48]; int paintGroupCount = 0;
  const char* paintCur = nullptr; uint64_t paintStart = 0; }
extern "C" void n3dsPaintGroup(const char* name) {
  if (!n3ds_prof::enabled()) return;
  const uint64_t now = svcGetSystemTick();
  if (paintCur) {
    int i = 0; while (i < paintGroupCount && paintGroups[i].name != paintCur) ++i;
    if (i == paintGroupCount && paintGroupCount < 48) paintGroups[paintGroupCount++] = {paintCur, 0, 0};
    if (i < paintGroupCount) { paintGroups[i].ticks += now - paintStart; ++paintGroups[i].calls; }
  }
  paintCur = name; paintStart = now;
}
static void logPaintGroups() {
  const double toMs = 1000.0 / double(SYSCLOCK_ARM11);
  char line[1024]; int n = std::snprintf(line, sizeof line, "[paint-prof] ms/frame:");
  for (int i = 0; i < paintGroupCount && n < int(sizeof line) - 40; ++i)
    n += std::snprintf(line + n, sizeof line - n, " %s %.2f", paintGroups[i].name, double(paintGroups[i].ticks) * toMs / 60.0);
  n3dsLog("%s\n", line);
  for (int i = 0; i < paintGroupCount; ++i) paintGroups[i].ticks = 0, paintGroups[i].calls = 0;
}
namespace n3ds_prof {
bool detailEnabled() { static const bool on = std::getenv("N3DS_PROF_DETAIL") != nullptr; return on; }
namespace {
struct Sample { uint64_t ticks = 0, calls = 0, frameTicks = 0, peak = 0; unsigned depth = 0; };
// Separate worker activity from the game thread that owns the frame scope.
thread_local Sample samples[Count];
thread_local unsigned frames = 0;
thread_local uint64_t totalFrames = 0, windowStartTick = 0;
const char* names[Count] = {"frame", "execute-all", "draw-all", "actor-execute", "actor-draw",
                           "J3D-calc", "J3D-entry", "J3D-viewCalc", "J3D-drawBuffer",
                           "main-loop", "audio", "painter", "after-draw", "particle-calc", "pad-read",
                           "collision", "foliage-exec", "foliage-draw", "bg-move", "bg-acch", "bg-ground", "bg-line", "bg-wall", "bg-sphere", "bg-shadow", "j3d-mat-load", "j3d-mat-tex", "j3d-shape-mtx", "j3d-shape-dl", "j3d-shape-arr", "scn-event", "scn-attention", "scn-detect", "scn-demo"};   // (Claude v111)   // (Claude v107)
// (Claude v109) per process-type ticks (game thread only)
struct ActorSlot { uint64_t exec = 0, draw = 0; uint32_t n = 0; };
ActorSlot actorSlots[1024];
void logActors() {
  int idx[12]; int k = 0;
  bool used[1024] = {};
  const double toMs = 1000.0 / double(SYSCLOCK_ARM11);
  for (; k < 12; ++k) {
    int best = -1; uint64_t bt = 0;
    for (int i = 0; i < 1024; ++i) { const uint64_t t = actorSlots[i].exec + actorSlots[i].draw; if (!used[i] && t > bt) { bt = t; best = i; } }
    if (best < 0) break;
    used[best] = true; idx[k] = best;
  }
  char line[700]; int p = std::snprintf(line, sizeof line, "[proc-prof] top process types (proc-name exec+draw ms/frame, instances):");
  for (int j = 0; j < k && p < int(sizeof line) - 40; ++j) {
    const auto& a = actorSlots[idx[j]];
    p += std::snprintf(line + p, sizeof line - p, " %d:%.2f+%.2f/%u", idx[j], double(a.exec) * toMs / 60.0, double(a.draw) * toMs / 60.0, a.n / 60u);
  }
  n3dsLog("%s\n", line);
  for (auto& a : actorSlots) a = ActorSlot{};
}
}  // namespace
void actorAdd(int procName, bool draw, uint64_t ticks) {
  auto& a = actorSlots[unsigned(procName) & 1023u];
  if (draw) a.draw += ticks; else { a.exec += ticks; ++a.n; }
}
uint64_t ProcScope::now() { return svcGetSystemTick(); }
ProcScope*& ProcScope::top() { static ProcScope* t = nullptr; return t; }
bool enabled() {
  static const bool on = [] {
    const char* value = std::getenv("N3DS_PROF_GAME");
    return value && !std::strcmp(value, "1");
  }();
  return on;
}
uint64_t begin(Phase phase) {
  ++samples[phase].depth;
  ++samples[phase].calls;
  const uint64_t tick = svcGetSystemTick();
  if (phase == Frame && frames == 0 && samples[Frame].depth == 1) windowStartTick = tick;
  return tick;
}
void end(Phase phase, uint64_t start) {
  const uint64_t now = svcGetSystemTick();
  auto& s = samples[phase];
  if (!s.depth) return;
  if (--s.depth == 0) { // same-category recursion is counted once, not twice
    s.ticks += now - start;
    s.frameTicks += now - start;
  }
  if (phase != Frame || s.depth) return;
  for (auto& sample : samples) {
    if (sample.frameTicks > sample.peak) sample.peak = sample.frameTicks;
    sample.frameTicks = 0;
  }
  ++totalFrames;
  if (++frames != 60) return;
  // End the measured window before logging. Categories overlap; do not sum them.
  n3dsLog("[game-prof] 60 game frames; inclusive wall ms/frame, peak-frame ms, calls/frame (overlapping categories); game-local frames %llu..%llu ticks %llu..%llu\n",
          static_cast<unsigned long long>(totalFrames - 59), static_cast<unsigned long long>(totalFrames),
          static_cast<unsigned long long>(windowStartTick), static_cast<unsigned long long>(now));
  const double toMs = 1000.0 / double(SYSCLOCK_ARM11);
  for (int i = 0; i < Count; ++i) {
    const auto& v = samples[i];
    n3dsLog("[game-prof] %-15s avg %.3f peak %.3f calls %.2f\n", names[i],
            double(v.ticks) * toMs / 60.0, double(v.peak) * toMs, double(v.calls) / 60.0);
  }
  for (auto& sample : samples) sample = Sample{};
  logActors();
  logPaintGroups();
  frames = 0;
}
}
