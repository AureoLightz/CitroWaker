// Retained geometry (Claude, 2026-11-10): a batch of GX draws whose inputs are identical to a batch of the previous frame is not decoded again.
//
// The decoded vertices of a draw are a function of its inputs: the draw opcode and primitive-run structure, the vertex format registers (VCD + the VAT of its
// format), the stream bytes (direct data and indices), the bindings of its indexed arrays (address, stride, size, endianness) and the bytes of those arrays that
// its indices reach. The retainer keeps all of these for every draw of every batch it publishes, and a draw of the next frame is skipped only when every one of
// them is equal; the array bytes are compared against a snapshot taken when the retained geometry was decoded (and versioned: a snapshot that saw different
// bytes since invalidates every record made with the old ones). Exactness does not depend on GXInvalidateVtxCache or on the game flushing caches: the bytes are
// compared at the time the draw is processed, which is when decoding would have read them.
//
// Cost: the comparison of each array's touched byte range is done once per process() call (the game cannot write memory while the fifo processes its bytes),
// which is ~0.2 MB per frame on the wide Outset fixture against ~2 MB of decoded output; stream bytes ~0.35 MB.
//
// Granularity: a sink's batch (gxbatch.h: consecutive draws merged under the same register state). A batch whose draws all match one retained batch, in order and
// in number, gets that batch's immutable decoded geometry (RetainedGeometry, shared) and is not decoded at all. A batch that diverges part-way is materialized:
// the matched prefix is copied from the retained geometry, the rest decoded as usual. Batching decisions are unchanged: skipped draws advance the batch's counts
// exactly as decoding would (GxDraw::retained / retainedTris).
//
// Thread model: one producer (the thread that runs GxFifo::process); RetainedGeometry is immutable once published and may be read by a render thread.
#pragma once
#include "clip_bounds.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "gxfifo.h"

namespace t2p {

// (Claude v122) matrixbatch's per-geometry facts (index range, the distinct matrix slots in first-use order), computed once by the render owner
struct GeometryPmMemo { int8_t state = 0; uint8_t why = 0, n = 0; int8_t k[10] = {}; };   // state 0: not yet, 1: valid, -1: refused (why)
struct RetainedGeometry {
  GxDraw d;                // the decoded batch (header + vertices + triangles); never changed after publication
  mutable ClipBounds clipBounds; // Only render owner writes this memo.
  mutable GeometryPmMemo pmMemo;   // (Claude v122) only the render owner touches it
  uint64_t id = 0;         // unique per geometry (render-side caches key on it)
  int maxPm = -1;          // maxPosMtx(d)
  size_t bytes = 0;        // heap held (vector capacities)
};

struct GxRetainStats {
  uint64_t draws = 0, skipped = 0, batches = 0, retainedBatches = 0, published = 0, materialized = 0, notRetainable = 0, closedOff = 0, overBudget = 0;
  uint64_t verifyBytes = 0, snapshotBytes = 0, streamBytes = 0;
  uint64_t shapeMatches = 0;   // (Claude v95) draws matched as shape references
  uint64_t shapeOffered = 0, shapeNewNoPrev = 0, shapeNewMiss = 0, shapeMidMiss = 0;   // (Claude v95 diag) shape draws offered; first-of-batch with no candidate key / candidates that failed; mid-batch mismatch
  // (Claude v97 diag) batches decoded this frame, by why they were not retained: their batches and decoded vertices
  // 0 not retainable, 1 no previous batch with this key, 2 first draw: header (cmd/count/format/parts) differs, 3 array bindings differ,
  // 4 stream bytes differ, 5 array bytes changed, 6 shape key differs, 7 mid-batch divergence (prefix matched), 8 over budget
  uint64_t missBatches[9] = {}, missVerts[9] = {};
  size_t heldBytes = 0;    // geometry + records + snapshots held for the next frame
};

class GxRetainer {
 public:
  GxRetainer();
  ~GxRetainer();
  void setBudget(size_t bytes) { budget_ = bytes; }   // geometry + records held; beyond it new batches are not published (they decode every frame)

  // A new batch (sink event) starts with this draw; `D` is its empty target. Returns true when its decoding is skipped (D.retained is set, D's counts are advanced
  // by the fifo). `chunk` = GxFifo::processSerial(). drawMaxPm receives the skipped draw's maxPosMtx.
  bool offer(const GxDrawInputs& in, GxDraw& D, bool newBatch, uint64_t chunk, int& drawMaxPm);
  // The draw just offered was decoded into D (vertices from firstVertex, triangles from firstTri).
  void decoded(const GxDrawInputs& in, const GxDraw& D, uint32_t firstVertex, size_t firstTri, bool arrayError, uint64_t chunk);
  // The open batch receives no more draws. D is its target: when the batch matched a retained one in full, D stays header-only and the shared geometry is
  // returned; when it was decoded and is published, D's vectors move into the returned geometry (D keeps its header); nullptr: D holds the decoded batch.
  std::shared_ptr<const RetainedGeometry> closeBatch(GxDraw& D, int maxPm);
  bool open() const { return open_; }
  std::shared_ptr<const RetainedGeometry> matchingGeometry()const{return mode_==Matching&&cand_?cand_->geo:nullptr;}
  // Every batch of the frame has been closed: what this frame used becomes what the next one can match.
  void endFrame();
  void clear();
  GxRetainStats takeStats() { GxRetainStats s = st_; st_ = GxRetainStats(); s.heldBytes = held_; return s; }

 private:
  struct Watch;
  struct Binding { const uint8_t* base; uint32_t stride, size; bool be; Watch* w; uint32_t version; };   // an indexed array as a draw saw it
  struct Range { uint32_t binding; uint32_t b0, b1; };   // bytes [b0, b1) of Batch::bindings[binding] that a draw's indices reach
  struct DrawRec {
    uint8_t cmd = 0; uint8_t nRange = 0; uint32_t vertices = 0, vsize = 0; uint32_t fmt[5] = {};
    uint32_t partOff = 0, nParts = 0;      // into Batch::parts
    uint32_t streamOff = 0;                // into Batch::stream (vertices * vsize bytes)
    uint32_t rangeOff = 0;                 // into Batch::ranges, one per indexed attribute in stream order
    int maxPm = -1;
    uint32_t vertsEnd = 0; uint32_t trisEnd = 0;   // the batch's counts after this draw
    uint64_t shapeKey = 0; uint32_t shapeDraw = 0;  // (Claude v95) a shape-reference draw: matched on these, no stream bytes or ranges kept
  };
  struct Batch {
    uint64_t key = 0;                      // keyOf(first draw)
    std::vector<DrawRec> recs;
    std::vector<uint32_t> parts;           // (prim << 24) | count
    std::vector<uint8_t> stream;
    std::vector<Binding> bindings;         // distinct bindings of the batch's draws
    std::vector<Range> ranges;
    std::shared_ptr<RetainedGeometry> geo;
    uint64_t usedFrame = ~0ull;
    size_t bytes = 0;
  };
  enum Mode { None, Matching, Fresh, Off };

  static bool retainable(const GxDrawInputs& in);
  static uint64_t keyOf(const GxDrawInputs& in);
  bool match(const Batch& b, const DrawRec& r, const GxDrawInputs& in, uint64_t chunk);
  bool verify(const Binding& bd, uint32_t b0, uint32_t b1, uint64_t chunk);
  Watch* snapshot(const uint8_t* base, uint32_t b0, uint32_t b1, uint64_t chunk, uint32_t& version);
  void record(const GxDrawInputs& in, const GxDraw& D, uint32_t firstVertex, uint64_t chunk);
  void materialize(GxDraw& D);
  void use(const std::shared_ptr<Batch>& b);

  std::unordered_map<uint64_t, std::vector<std::shared_ptr<Batch>>> prev_;   // the previous frame's batches by key of their first draw
  std::vector<std::shared_ptr<Batch>> cur_;                                   // batches used or published this frame
  std::unordered_map<const uint8_t*, std::unique_ptr<Watch>> watches_;
  bool open_ = false;
  Mode mode_ = None;
  std::shared_ptr<Batch> cand_;           // Matching: the retained batch
  uint32_t cursor_ = 0;                   // Matching: draws of cand_ matched
  std::shared_ptr<Batch> building_;       // Fresh: records of the draws decoded so far
  uint64_t frame_ = 0, nextId_ = 0;
  size_t budget_ = 8u << 20, held_ = 0, heldNext_ = 0;
  GxRetainStats st_;
  uint8_t lastFail_ = 0, reason_ = 0;   // (Claude v97 diag)
};

}  // namespace t2p
