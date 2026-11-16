#include "internal.h"
#include <c3d/base.h>
#include <c3d/renderqueue.h>
#include <stdlib.h>

static C3D_RenderTarget *firstTarget, *lastTarget;
static C3D_RenderTarget *linkedTarget[3];

static TickCounter gpuTime, cpuTime;

static bool inFrame, inSafeTransfer, measureGpuTime;
static bool needSwapTop, needSwapBot, isTopStereo;
static float framerate = 60.0f;
static float framerateCounter[2] = { 60.0f, 60.0f };
static u32 frameCounter[2];
static void (* frameEndCb)(void*);
static void* frameEndCbData;

static void C3Di_RenderTargetDestroy(C3D_RenderTarget* target);

static bool framerateLimit(int id)
{
	framerateCounter[id] -= framerate;
	if (framerateCounter[id] <= 0.0f)
	{
		framerateCounter[id] += 60.0f;
		return true;
	}
	return false;
}

static void onVBlank0(C3D_UNUSED void* unused)
{
	if (framerateLimit(0))
		frameCounter[0]++;
}

static void onVBlank1(C3D_UNUSED void* unused)
{
	if (framerateLimit(1))
		frameCounter[1]++;
}

static volatile bool staleGuard;   // (Claude v120) see C3Dx_FrameKick
static void onQueueFinish(gxCmdQueue_s* queue)
{
	// (Claude v120) libctru clears isRunning before it calls this (outside its lock), so after a kicked frame a drain of the mid-frame
	// lists can still be on its way here while C3D_FrameEnd has queued the last list and set the swap flags: that callback must not
	// swap. Until the frame's own entries have all run (lastEntry == numEntries) callbacks are ignored.
	if (staleGuard && !inSafeTransfer)
	{
		__sync_synchronize();
		if (queue->lastEntry < queue->numEntries)
			return;
		staleGuard = false;
	}
	if (measureGpuTime)
	{
		osTickCounterUpdate(&gpuTime);
		measureGpuTime = false;
	}
	if (inSafeTransfer)
	{
		inSafeTransfer = false;
		if (inFrame)
		{
			gxCmdQueueStop(queue);
			gxCmdQueueClear(queue);
		}
	}
	else
	{
		if (needSwapTop)
		{
			gfxScreenSwapBuffers(GFX_TOP, isTopStereo);
			needSwapTop = false;
		}
		if (needSwapBot)
		{
			gfxScreenSwapBuffers(GFX_BOTTOM, false);
			needSwapBot = false;
		}
	}
}

void C3D_FrameSync(void)
{
	u32 cur[2];
	u32 start[2] = { frameCounter[0], frameCounter[1] };
	do
	{
		gspWaitForAnyEvent();
		cur[0] = frameCounter[0];
		cur[1] = frameCounter[1];
	} while (cur[0]==start[0] || cur[1]==start[1]);
}

u32 C3D_FrameCounter(int id)
{
	return frameCounter[id];
}

static bool C3Di_WaitAndClearQueue(s64 timeout)
{
	gxCmdQueue_s* queue = &C3Di_GetContext()->gxQueue;
	if (!gxCmdQueueWait(queue, timeout))
		return false;
	gxCmdQueueStop(queue);
	gxCmdQueueClear(queue);
	return true;
}

void C3Di_RenderQueueEnableVBlank(void)
{
	gspSetEventCallback(GSPGPU_EVENT_VBlank0, onVBlank0, NULL, false);
	gspSetEventCallback(GSPGPU_EVENT_VBlank1, onVBlank1, NULL, false);
}

void C3Di_RenderQueueDisableVBlank(void)
{
	gspSetEventCallback(GSPGPU_EVENT_VBlank0, NULL, NULL, false);
	gspSetEventCallback(GSPGPU_EVENT_VBlank1, NULL, NULL, false);
}

void C3Di_RenderQueueInit(void)
{
	C3D_Context* ctx = C3Di_GetContext();

	C3Di_RenderQueueEnableVBlank();

	GX_BindQueue(&ctx->gxQueue);
	gxCmdQueueSetCallback(&ctx->gxQueue, onQueueFinish, NULL);
	gxCmdQueueRun(&ctx->gxQueue);
}

void C3Di_RenderQueueExit(void)
{
	int i;
	C3D_RenderTarget *a, *next;

	C3Di_WaitAndClearQueue(-1);
	gxCmdQueueSetCallback(&C3Di_GetContext()->gxQueue, NULL, NULL);
	GX_BindQueue(NULL);

	C3Di_RenderQueueDisableVBlank();

	for (i = 0; i < 3; i ++)
		linkedTarget[i] = NULL;

	for (a = firstTarget; a; a = next)
	{
		next = a->next;
		C3Di_RenderTargetDestroy(a);
	}
}

void C3Di_RenderQueueWaitDone(void)
{
	C3Di_WaitAndClearQueue(-1);
}

float C3D_FrameRate(float fps)
{
	float old = framerate;
	if (fps > 0.0f && fps <= 60.0f)
	{
		framerate = fps;
		framerateCounter[0] = fps;
		framerateCounter[1] = fps;
	}
	return old;
}

bool C3D_FrameBegin(u8 flags)
{
	C3D_Context* ctx = C3Di_GetContext();
	if (inFrame) return false;

	if (flags & C3D_FRAME_SYNCDRAW)
		C3D_FrameSync();
	if (!C3Di_WaitAndClearQueue((flags & C3D_FRAME_NONBLOCK) ? 0 : -1))
		return false;
	// (Claude v120) a kicked frame's own swap callback may still be on its way (isRunning is cleared first): let it run before this
	// frame adds entries (it checks lastEntry against numEntries); bounded, 5 ms
	for (int i = 0; staleGuard && i < 100; i++)
		svcSleepThread(50000);
	staleGuard = false;

	inFrame = true;
	osTickCounterStart(&cpuTime);
	GPUCMD_SetBuffer(ctx->cmdBuf, ctx->cmdBufSize, 0);
	return true;
}

bool C3D_FrameDrawOn(C3D_RenderTarget* target)
{
	if (!inFrame) return false;

	target->used = true;
	C3D_SetFrameBuf(&target->frameBuf);
	C3D_SetViewport(0, 0, target->frameBuf.width, target->frameBuf.height);
	return true;
}

// (Claude v120) mid-frame GPU start: citro3d queues a frame's command lists (splits, transfers) and only runs the queue at C3D_FrameEnd,
// so the GPU worked after the CPU had built the whole frame (console: ~29 ms GPU after ~45 ms CPU, and the next C3D_FrameBegin waited
// for it). C3Dx_FrameKick runs the queue now: what is queued (the caller split the command list) starts on the GPU while the CPU builds
// the rest; later queued entries start as they are added. C3D_FrameEnd first waits for the queue to go idle and stops it, then queues the
// last list, the transfers and the swap exactly as before (no race with the swap flags).
static bool kicked;
bool C3Dx_FrameKick(void)
{
	gxCmdQueue_s* q = &C3Di_GetContext()->gxQueue;
	if (!inFrame || q->numEntries + 16 > q->maxEntries) return false;
	kicked = true;
	gxCmdQueueRun(q);
	return true;
}

void C3D_FrameSplit(u8 flags)
{
	u32 *cmdBuf, cmdBufSize;
	if (!inFrame) return;
	if (C3Di_SplitFrame(&cmdBuf, &cmdBufSize))
		GX_ProcessCommandList(cmdBuf, cmdBufSize*4, flags);
}

void C3D_FrameEnd(u8 flags)
{
	C3D_Context* ctx = C3Di_GetContext();
	if (!inFrame) return;

	if (frameEndCb)
		frameEndCb(frameEndCbData);

	if (kicked)
	{
		gxCmdQueueWait(&ctx->gxQueue, -1);
		staleGuard = true;
		__sync_synchronize();
		gxCmdQueueStop(&ctx->gxQueue);
		kicked = false;
	}

	C3D_FrameSplit(flags);
	GPUCMD_SetBuffer(NULL, 0, 0);
	osTickCounterUpdate(&cpuTime);
	inFrame = false;

	// Flush the entire linear memory if the user did not explicitly mandate to flush the command list
	if (!(flags & GX_CMDLIST_FLUSH))
	{
		extern u32 __ctru_linear_heap;
		extern u32 __ctru_linear_heap_size;
		GSPGPU_FlushDataCache((void*)__ctru_linear_heap, __ctru_linear_heap_size);
	}

	int i;
	C3D_RenderTarget* target;
	isTopStereo = false;
	for (i = 2; i >= 0; i --)
	{
		target = linkedTarget[i];
		if (!target || !target->used)
			continue;
		target->used = false;
		C3D_FrameBufTransfer(&target->frameBuf, target->screen, target->side, target->transferFlags);
		if (target->screen == GFX_TOP)
		{
			needSwapTop = true;
			if (target->side == GFX_RIGHT)
				isTopStereo = true;
		}
		else if (target->screen == GFX_BOTTOM)
			needSwapBot = true;
	}

	measureGpuTime = true;
	osTickCounterStart(&gpuTime);
	gxCmdQueueRun(&ctx->gxQueue);
	if (ctx->gxQueue.lastEntry >= ctx->gxQueue.numEntries)
		staleGuard = false;   // (Claude v120) nothing was queued: no callback will come
}

void C3D_FrameEndHook(void (* hook)(void*), void* param)
{
	frameEndCb = hook;
	frameEndCbData = param;
}

float C3D_GetDrawingTime(void)
{
	return osTickCounterRead(&gpuTime);
}

float C3D_GetProcessingTime(void)
{
	return osTickCounterRead(&cpuTime);
}

static C3D_RenderTarget* C3Di_RenderTargetNew(void)
{
	C3D_RenderTarget* target = (C3D_RenderTarget*)malloc(sizeof(C3D_RenderTarget));
	if (!target) return NULL;
	memset(target, 0, sizeof(C3D_RenderTarget));
	return target;
}

static void C3Di_RenderTargetFinishInit(C3D_RenderTarget* target)
{
	target->prev = lastTarget;
	target->next = NULL;
	if (lastTarget)
		lastTarget->next = target;
	if (!firstTarget)
		firstTarget = target;
	lastTarget = target;
}

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, C3D_DEPTHTYPE depthFmt)
{
	GPU_DEPTHBUF depthFmtReal = GPU_RB_DEPTH16;
	void* depthBuf = NULL;
	void* colorBuf = vramAlloc(C3D_CalcColorBufSize(width,height,colorFmt));
	if (!colorBuf) goto _fail0;
	if (C3D_DEPTHTYPE_OK(depthFmt))
	{
		depthFmtReal = C3D_DEPTHTYPE_VAL(depthFmt);
		size_t depthSize = C3D_CalcDepthBufSize(width,height,depthFmtReal);
		vramAllocPos vramBank = addrGetVRAMBank(colorBuf);
		depthBuf = vramAllocAt(depthSize, vramBank ^ VRAM_ALLOC_ANY); // Attempt opposite bank first...
		if (!depthBuf) depthBuf = vramAllocAt(depthSize, vramBank); // ... if that fails, attempt same bank
		if (!depthBuf) goto _fail1;
	}

	C3D_RenderTarget* target = C3Di_RenderTargetNew();
	if (!target) goto _fail2;

	C3D_FrameBuf* fb = &target->frameBuf;
	C3D_FrameBufAttrib(fb, width, height, false);
	C3D_FrameBufColor(fb, colorBuf, colorFmt);
	target->ownsColor = true;
	if (depthBuf)
	{
		C3D_FrameBufDepth(fb, depthBuf, depthFmtReal);
		target->ownsDepth = true;
	}
	C3Di_RenderTargetFinishInit(target);
	return target;

_fail2:
	if (depthBuf) vramFree(depthBuf);
_fail1:
	vramFree(colorBuf);
_fail0:
	return NULL;
}

C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level, C3D_DEPTHTYPE depthFmt)
{
	if (!addrIsVRAM(tex->data)) return NULL; // Render targets must be in VRAM
	C3D_RenderTarget* target = C3Di_RenderTargetNew();
	if (!target) return NULL;

	C3D_FrameBuf* fb = &target->frameBuf;
	C3D_FrameBufTex(fb, tex, face, level);

	if (C3D_DEPTHTYPE_OK(depthFmt))
	{
		GPU_DEPTHBUF depthFmtReal = C3D_DEPTHTYPE_VAL(depthFmt);
		size_t depthSize = C3D_CalcDepthBufSize(fb->width,fb->height,depthFmtReal);
		vramAllocPos vramBank = addrGetVRAMBank(tex->data);
		void* depthBuf = vramAllocAt(depthSize, vramBank ^ VRAM_ALLOC_ANY); // Attempt opposite bank first...
		if (!depthBuf) depthBuf = vramAllocAt(depthSize, vramBank); // ... if that fails, attempt same bank
		if (!depthBuf)
		{
			free(target);
			return NULL;
		}

		C3D_FrameBufDepth(fb, depthBuf, depthFmtReal);
		target->ownsDepth = true;
	}

	C3Di_RenderTargetFinishInit(target);
	return target;
}

void C3Di_RenderTargetDestroy(C3D_RenderTarget* target)
{
	if (target->ownsColor)
		vramFree(target->frameBuf.colorBuf);
	if (target->ownsDepth)
		vramFree(target->frameBuf.depthBuf);

	C3D_RenderTarget** prevNext = target->prev ? &target->prev->next : &firstTarget;
	C3D_RenderTarget** nextPrev = target->next ? &target->next->prev : &lastTarget;
	*prevNext = target->next;
	*nextPrev = target->prev;
	free(target);
}

void C3D_RenderTargetDelete(C3D_RenderTarget* target)
{
	if (inFrame)
		svcBreak(USERBREAK_PANIC); // Shouldn't happen.
	if (target->linked)
		C3D_RenderTargetDetachOutput(target);
	else
		C3Di_WaitAndClearQueue(-1);
	C3Di_RenderTargetDestroy(target);
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags)
{
	int id = 0;
	if (screen==GFX_BOTTOM) id = 2;
	else if (side==GFX_RIGHT) id = 1;
	if (linkedTarget[id])
	{
		linkedTarget[id]->linked = false;
		if (!inFrame)
			C3Di_WaitAndClearQueue(-1);
	}
	linkedTarget[id] = target;
	if (target)
	{
		target->linked = true;
		target->transferFlags = transferFlags;
		target->screen = screen;
		target->side = side;
	}
}

static void C3Di_SafeDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags)
{
	C3Di_WaitAndClearQueue(-1);
	inSafeTransfer = true;
	GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
	gxCmdQueueRun(&C3Di_GetContext()->gxQueue);
}

static void C3Di_SafeTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags)
{
	C3Di_WaitAndClearQueue(-1);
	inSafeTransfer = true;
	GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
	gxCmdQueueRun(&C3Di_GetContext()->gxQueue);
}

static void C3Di_SafeMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e, u16 control1)
{
	C3Di_WaitAndClearQueue(-1);
	inSafeTransfer = true;
	GX_MemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
	gxCmdQueueRun(&C3Di_GetContext()->gxQueue);
}

void C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags)
{
	if (inFrame)
	{
		C3D_FrameSplit(0);
		GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
	} else
	{
		C3Di_SafeDisplayTransfer(inadr, indim, outadr, outdim, flags);
		gspWaitForPPF();
	}
}

void C3D_SyncTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags)
{
	if (inFrame)
	{
		C3D_FrameSplit(0);
		GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
	} else
	{
		C3Di_SafeTextureCopy(inadr, indim, outadr, outdim, size, flags);
		gspWaitForPPF();
	}
}

void C3D_SyncMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e, u16 control1)
{
	if (inFrame)
	{
		C3D_FrameSplit(0);
		GX_MemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
	} else
	{
		C3Di_SafeMemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
		gspWaitForPSC0();
	}
}
