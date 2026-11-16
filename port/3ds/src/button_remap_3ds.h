// Optional /3ds/ww3ds/buttons.ini; read once at startup, never per frame.
#pragma once
#include <dolphin/pad.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include "n3ds.h"

namespace n3ds_buttons {
struct Name { const char* name; uint32_t value; };
inline constexpr Name sources[] = {
  {"A", N3DS_KEY_A}, {"B", N3DS_KEY_B}, {"X", N3DS_KEY_X}, {"Y", N3DS_KEY_Y},
  {"L", N3DS_KEY_L}, {"R", N3DS_KEY_R}, {"ZL", N3DS_KEY_ZL}, {"ZR", N3DS_KEY_ZR},
  {"START", N3DS_KEY_START}, {"SELECT", N3DS_KEY_SELECT},
  {"UP", N3DS_KEY_DUP}, {"DOWN", N3DS_KEY_DDOWN},
  {"LEFT", N3DS_KEY_DLEFT}, {"RIGHT", N3DS_KEY_DRIGHT},
};
inline constexpr Name targets[] = {
  {"NONE", 0}, {"A", PAD_BUTTON_A}, {"B", PAD_BUTTON_B},
  {"X", PAD_BUTTON_X}, {"Y", PAD_BUTTON_Y},
  {"L", PAD_TRIGGER_L}, {"R", PAD_TRIGGER_R}, {"Z", PAD_TRIGGER_Z},
  {"START", PAD_BUTTON_START}, {"UP", PAD_BUTTON_UP}, {"DOWN", PAD_BUTTON_DOWN},
  {"LEFT", PAD_BUTTON_LEFT}, {"RIGHT", PAD_BUTTON_RIGHT},
};
inline char* trim(char* p) {
  while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
  char* end = p + std::strlen(p);
  while (end > p && std::isspace(static_cast<unsigned char>(end[-1]))) --end;
  *end = 0;
  return p;
}
inline bool lookup(const Name* table, unsigned count, const char* name, uint32_t& value) {
  for (unsigned i = 0; i < count; ++i)
    if (!std::strcmp(table[i].name, name)) { value = table[i].value; return true; }
  return false;
}
class Mapping {
  struct Entry { uint32_t source, target; };
  Entry entries[32] = {
    {N3DS_KEY_A, PAD_BUTTON_A}, {N3DS_KEY_B, PAD_BUTTON_B},
    {N3DS_KEY_X, PAD_BUTTON_X}, {N3DS_KEY_Y, PAD_BUTTON_Y},
    {N3DS_KEY_L, PAD_TRIGGER_L}, {N3DS_KEY_R, PAD_TRIGGER_R},
    {N3DS_KEY_ZL, PAD_TRIGGER_Z}, {N3DS_KEY_ZR, PAD_TRIGGER_Z},
    {N3DS_KEY_START, PAD_BUTTON_START}, {N3DS_KEY_SELECT, 0},
    {N3DS_KEY_DUP, PAD_BUTTON_UP}, {N3DS_KEY_DDOWN, PAD_BUTTON_DOWN},
    {N3DS_KEY_DLEFT, PAD_BUTTON_LEFT}, {N3DS_KEY_DRIGHT, PAD_BUTTON_RIGHT},
    {N3DS_KEY_SELECT | N3DS_KEY_START, PAD_TRIGGER_Z},
  };
  unsigned count = 15;
public:
  uint32_t apply(uint32_t held) const {
    uint32_t output = 0, consumed = 0;
    // Chords take precedence over individual buttons, including NONE chords.
    for (unsigned i = 0; i < count; ++i) {
      const auto& e = entries[i];
      if ((e.source & (e.source - 1)) && (held & e.source) == e.source) {
        output |= e.target; consumed |= e.source;
      }
    }
    for (unsigned i = 0; i < count; ++i) {
      const auto& e = entries[i];
      if (!(e.source & (e.source - 1)) && (held & e.source) && !(consumed & e.source))
        output |= e.target;
    }
    return output;
  }
  bool parseLine(char* line) {
    if (char* comment = std::strchr(line, '#')) *comment = 0;
    char* text = trim(line);
    if (!*text) return true;
    char* eq = std::strchr(text, '=');
    if (!eq || std::strchr(eq + 1, '=')) return false;
    *eq = 0;
    uint32_t target, source = 0;
    if (!lookup(targets, sizeof targets / sizeof targets[0], trim(eq + 1), target)) return false;
    char* part = trim(text);
    for (;;) {
      char* plus = std::strchr(part, '+');
      if (plus) *plus = 0;
      uint32_t key;
      if (!lookup(sources, sizeof sources / sizeof sources[0], trim(part), key) || (source & key)) return false;
      source |= key;
      if (!plus) break;
      part = plus + 1;
    }
    for (unsigned i = 0; i < count; ++i)
      if (entries[i].source == source) { entries[i].target = target; return true; }
    if (count == sizeof entries / sizeof entries[0]) return false;
    entries[count++] = {source, target};
    return true;
  }
  void load(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return; // absent file: stock controls
    char line[256]; unsigned number = 0;
    while (std::fgets(line, sizeof line, f)) {
      ++number;
      // Reject overlong lines in full instead of parsing their truncated tail.
      if (!std::strchr(line, '\n') && !std::feof(f)) {
        int ch = std::fgetc(f);
        if (ch != EOF) {
          while (ch != '\n' && ch != EOF) ch = std::fgetc(f);
          n3dsLog("[buttons] ignored overlong line %u\n", number);
          continue;
        }
      }
      if (!parseLine(line)) n3dsLog("[buttons] ignored invalid line %u\n", number);
    }
    std::fclose(f);
  }
};
inline Mapping mapping;
inline void init() {
  static bool loaded = false;
  if (!loaded) { loaded = true; mapping.load(N3DS_ROOT "/buttons.ini"); }
}
} // namespace n3ds_buttons
