// Material bundle: translator results computed offline, looked up at run time.
//
// Translating one TEV material costs about 55 ms on a PC on average (the search for PICA stage sequences, 90 % of compileDraw), so about a second or more on the 3DS's ARM11, and a
// level has hundreds of materials: the game cannot translate while it plays. The bundle is the product of running the translator over every material of the game once, offline:
//   * translations: translate()'s result (PICA program, per-stage notes, verification statistics) for a canonical key of the TEV material (the stages as the search sees them
//     -- texmap is already a PICA unit -- and the options that steer it);
//   * approximations: what the approximate rung chose for a material that cannot be drawn exactly (approx.h), keyed by the GX material, so a run-time compile with a small
//     approxEffort still gets the degradation the offline search with a large one found.
// translate() and compileApprox() look in the installed bundle first and search only on a miss, so everything else compileDraw does (cut search, vertex shader, state) stays
// the same code on the same results: a draw compiled with the bundle is the draw compiled without it (tests/test_bundle.cpp proves it over the whole census). A material the
// bundle does not know (effects the game builds with GX calls rather than from its assets) is translated at run time as before and, when a recorder is installed, kept so a later
// run can write it into an extension bundle.
//
// The file is little endian, self-checking (magic, version, a 64 bit fingerprint of the translator sources, an FNV-1a checksum) and read with bounds checks: a damaged, truncated or
// stale file is refused as a whole and the program falls back to searching; it can never crash the loader (tests/test_bundle.cpp truncates and corrupts every byte).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "translate.h"

namespace t2p {

// ---------------------------------------------------------------- byte I/O
class ByteWriter {
 public:
  std::vector<uint8_t> buf;
  void u8(uint32_t v) { buf.push_back(uint8_t(v)); }
  void u16(uint32_t v) { u8(v); u8(v >> 8); }
  void u32(uint32_t v) { u16(v); u16(v >> 16); }
  void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
  void i32(int32_t v) { u32(uint32_t(v)); }
  void f32(float v);
  void f64(double v);
  void str(const std::string& s) { u32(uint32_t(s.size())); buf.insert(buf.end(), s.begin(), s.end()); }
};

// After a read past the end (or a bad value) ok() is false for good and every read returns 0.
class ByteReader {
 public:
  ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  size_t remaining() const { return n_ - at_; }
  void fail() { ok_ = false; at_ = n_; }
  uint8_t u8() { if (at_ >= n_) { fail(); return 0; } return p_[at_++]; }
  uint16_t u16() { const uint32_t a = u8(); return uint16_t(a | u8() << 8); }
  uint32_t u32() { const uint32_t a = u16(); return a | uint32_t(u16()) << 16; }
  uint64_t u64() { const uint64_t a = u32(); return a | uint64_t(u32()) << 32; }
  int32_t i32() { return int32_t(u32()); }
  float f32();
  double f64();
  std::string str(size_t maxLen = 1 << 20);
  // a count that must be at most `max` and cheap enough for the data left (each element takes at least `minBytes`)
  uint32_t count(uint32_t max, size_t minBytes);

 private:
  const uint8_t* p_;
  size_t n_, at_ = 0;
  bool ok_ = true;
};

void putTevMaterial(ByteWriter&, const TevMaterial&, bool withTexcoord);
bool getTevMaterial(ByteReader&, TevMaterial&);
void putTranslation(ByteWriter&, const Translation&);
bool getTranslation(ByteReader&, Translation&);

// ---------------------------------------------------------------- keys
// translate(): the stages (not the texcoord, which only draw.cpp reads) and the options translate() reads. Options::verifySamples is left out on purpose: it sets how many
// samples the final statistics use, not what is chosen, and the offline run uses the large default.
std::string translateKey(const TevMaterial&, const Options&);
// compileApprox(): the GX material as the draw state holds it (texmap, texcoord and all).
std::string approxKey(const TevMaterial&);

struct ApproxEntry {
  TevMaterial material;   // the degraded material (when !relaxed)
  bool relaxed = false;   // instead: the original material with the translator's error limit lifted
  std::string note;       // the draw's first note, verbatim
};

class MaterialBundle {
 public:
  const Translation* findTranslation(const std::string& key) const;
  const ApproxEntry* findApprox(const std::string& key) const;
  // The first result stored under a key stays (searches are deterministic, so a second one is the same).
  void addTranslation(const std::string& key, const Translation&);
  void addApprox(const std::string& key, const ApproxEntry&);
  size_t translations() const { return tr_.size(); }
  size_t approximations() const { return ap_.size(); }

  std::vector<uint8_t> serialize(uint64_t fingerprint) const;   // entries in key order, so equal bundles give equal files
  // Merges a file into this bundle. On any problem (not our magic / version, another translator's fingerprint, truncated, damaged) nothing is merged and `err` says why.
  bool load(const uint8_t* data, size_t size, uint64_t fingerprint, std::string* err = nullptr);

 private:
  std::unordered_map<std::string, Translation> tr_;
  std::unordered_map<std::string, ApproxEntry> ap_;
};

// ---------------------------------------------------------------- hooks (process wide, single threaded)
// lookup: consulted before searching. recorder: receives every result that had to be searched (what the lookup lacked).
void setBundleLookup(const MaterialBundle*);
void setBundleRecorder(MaterialBundle*);
const MaterialBundle* bundleLookup();
MaterialBundle* bundleRecorder();

struct BundleStats {
  uint64_t hits = 0, misses = 0;   // translate() calls found / not found in the lookup bundle (counted only while one is installed)
  double missSeconds = 0;          // wall time spent searching for the misses
  uint64_t approxHits = 0;
};
BundleStats bundleStats();
void resetBundleStats();
void bundleCountHit();                  // used by translate()
void bundleCountMiss(double seconds);
void bundleCountApproxHit();

// 64 bit fingerprint of the translator sources, passed to the compiler by the build scripts (-DT2P_SRC_FINGERPRINT=0x...ull; tools/fingerprint.sh); 0 in a build without it.
uint64_t sourceFingerprint();

}  // namespace t2p
