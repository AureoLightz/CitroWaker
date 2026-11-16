// Draw batching. GameCube models arrive as many short triangle strips (Wind Waker: ~16,000 strips of 4..15 vertices per frame), each its own GX draw command, and the GPU
// takes them at no extra cost. Every PICA draw costs a full compile / plan / submit on the ARM11, so consecutive draws made with identical register state are merged into one:
// their vertices are concatenated and their (already assembled) triangle lists appended. GX rasterises a draw's triangles in order, and the merged draw keeps that order, so the
// picture is the same (exception: two-pass materials, multipass.h, draw all first passes of the batch before all second passes; overlapping fragments of one batch may differ).
//
// "Identical register state" = GxFifo::stateGeneration() unchanged between the two draws, plus the same vertex descriptor (a sink that merges must check both through canAppend).
// Game-agnostic.
#pragma once
#include <cstdint>

#include "gxfifo.h"

namespace t2p {
// Reclaim large unused vector tails after the producer closes a batch. No geometry changes.
size_t compactDrawStorage(GxDraw&);


// Most vertices a merged draw may hold. 16-bit indices address 65536; the backend's per-frame arena is the tighter limit (stride up to ~60 bytes), so batches stay well below it.
constexpr uint32_t kBatchMaxVertices = 8192;

// `into` and `d` have the same vertex attributes, both are triangle draws, and the sum fits `maxVertices`.
bool canAppend(const GxDraw& into, const GxDraw& d, uint32_t maxVertices = kBatchMaxVertices);
// The same before decoding `d` (only its header: prim, vertexCount, vcd, unsupportedPrim): GxSink::appendTarget.
bool canAppendHeader(const GxDraw& into, const GxDraw& header, uint32_t maxVertices = kBatchMaxVertices);
// Highest position matrix number named by the per-vertex indices from `first` on (-1: none).
int maxPosMtxFrom(const GxDraw&, uint32_t first);
// Appends d's vertices and triangles to `into` (canAppend must be true). `into` becomes a triangle list.
void appendDraw(GxDraw& into, const GxDraw& d);
// Highest position matrix number a draw's per-vertex indices name (-1: none).
int maxPosMtx(const GxDraw&);

}  // namespace t2p
