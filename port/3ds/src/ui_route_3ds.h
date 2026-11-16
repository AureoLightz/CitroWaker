// (Claude, work split v2) Bottom-screen routing of the original minimap draws. Receiving side owned by Claude; game-side callers by ChatGPT.
//
// Contract:
//  - Call n3dsUiMapBegin right before a queued map object EXECUTES its GX draws and n3dsUiMapEnd right after (not around dMap_c::draw, which only enqueues).
//  - (left, top, width, height): destination rectangle on the bottom screen in bottom-screen pixels (320x240, origin top-left).
//    The source is the original top-screen 2D coordinate space (the draws are unchanged); the receiver applies the source->destination affine transform.
//  - Begin returns true: the GX draws up to the matching End go to the bottom-screen target, in stream order (an ordered route marker in the GX
//    stream, recorded with the frame; no global state shared with core 3). Begin returns false: draw exactly as before (top screen), and do NOT call End.
//  - Begin/End do not nest. Canonical GX state is shared and unchanged by routing; EFB alpha written inside a routed span stays in the bottom target
//    (so DST_ALPHA blends of later routed map objects read it); the top screen's alpha is untouched.
//  - Off unless performance.ini ui_map_bottom=1. Current build: a stub, Begin always returns false.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
bool n3dsUiMapBegin(int left, int top, int width, int height);
void n3dsUiMapEnd(void);
#ifdef __cplusplus
}
#endif
