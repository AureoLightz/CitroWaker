// (Claude v119) Recorded draws: CitroWaker's draw replay sends a draw that repeats from the previous frame as pre-built command words
// instead of going through the citro3d setters and C3Di_UpdateContext. This file captures the context state of a draw that was just
// issued, writes each state group's commands (made by citro3d's own bind functions, so the words are exactly what an update would emit
// for that state) into a caller buffer, and puts a group's state back into the context without dirty flags, so that anything citro3d
// re-emits later (after a frame split, a target change, a program bind) is the state the GPU really has.
#include "internal.h"
#include <string.h>

// shared: a material's state (attributes, effect + fog table, TEV) - many recorded draws refer to one copy; own: one draw's buffers,
// texture units and scissor
typedef struct
{
	C3D_AttrInfo attrInfo;
	C3D_Effect effect;
	C3D_FogLut* fogLut;
	C3D_TexEnv texEnv[6];
	u32 texEnvBuf, texEnvBufClr, fogClr;
} C3Dx_Shared;
typedef struct
{
	C3D_BufInfo bufInfo;
	C3D_Tex tex[3];
	u8 texBound[3];
	u32 texConfig, texShadow;
	u32 scissor[3];
} C3Dx_Own;

size_t C3Dx_SharedSize(void) { return sizeof(C3Dx_Shared); }
size_t C3Dx_OwnSize(void) { return sizeof(C3Dx_Own); }

// false when the context has state of its own still to emit (C3Di_UpdateContext first)
bool C3Dx_Clean(void)
{
	C3D_Context* ctx = C3Di_GetContext();
	return !(ctx->flags & ~(u32)(C3DiF_Active | C3DiF_DrawUsed)) && !ctx->fixedAttribDirty && !ctx->lightEnv && !ctx->procTex &&
		(ctx->texEnvBuf & 7) != GPU_GAS;
}

void C3Dx_Capture(void* sharedOut, void* ownOut)
{
	C3D_Context* ctx = C3Di_GetContext();
	C3Dx_Shared* s = (C3Dx_Shared*)sharedOut;
	C3Dx_Own* o = (C3Dx_Own*)ownOut;
	memset(s, 0, sizeof *s); memset(o, 0, sizeof *o);
	s->attrInfo = ctx->attrInfo;
	s->effect = ctx->effect;
	s->fogLut = ctx->fogLut;
	memcpy(s->texEnv, ctx->texEnv, sizeof s->texEnv);
	s->texEnvBuf = ctx->texEnvBuf; s->texEnvBufClr = ctx->texEnvBufClr; s->fogClr = ctx->fogClr;
	o->bufInfo = ctx->bufInfo;
	for (int i = 0; i < 3; i++)
		if (ctx->tex[i]) { o->tex[i] = *ctx->tex[i]; o->texBound[i] = 1; }
	o->texConfig = ctx->texConfig & ~BIT(16); o->texShadow = ctx->texShadow;
	memcpy(o->scissor, ctx->scissor, sizeof o->scissor);
}

static void drawBody(int count, u32 pa, u32 base)
{   // C3Di_DrawElementsBody (drawElements.c), GPU_TRIANGLES with 16-bit indices
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM);
	GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
	GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, (pa - base) | (1u << 31));
	GPUCMD_AddWrite(GPUREG_NUMVERTICES, count);
	GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, 0);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
	GPUCMD_AddWrite(GPUREG_DRAWELEMENTS, 1);
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
	GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
}

// Group commands into out[0..cap) (cap must hold them: 2048 words is always enough). Returns the word count (even), 0 when the indices
// lie below the vertex base (no draw). group: 0 attributes, 1 buffers, 2 effect (+ fog table when fog is on), 3 TEV stages, 4 TEV buffer +
// fog colour, 5 texture units, 6 scissor, 7 draw (units: TEXUNIT_CONFIG byte 0 the backend writes; count / indices: the index list).
u32 C3Dx_Emit(const void* shared, const void* own, int group, u32* out, u32 cap, const u32* fogLutData, u32 units, int count, const void* indices)
{
	const C3Dx_Shared* s = (const C3Dx_Shared*)shared;
	const C3Dx_Own* o = (const C3Dx_Own*)own;
	u32* oldBuf; u32 oldSize, oldOffset;
	GPUCMD_GetBuffer(&oldBuf, &oldSize, &oldOffset);
	GPUCMD_SetBuffer(out, cap, 0);
	bool ok = true;
	switch (group)
	{
	case 0: { C3D_AttrInfo a = s->attrInfo; C3Di_AttrInfoBind(&a); break; }
	case 1: { C3D_BufInfo b = o->bufInfo; C3Di_BufInfoBind(&b); break; }
	case 2:
	{
		C3D_Effect e = s->effect; C3Di_EffectBind(&e);
		if ((s->texEnvBuf & 7) != GPU_NO_FOG && fogLutData)
		{
			GPUCMD_AddWrite(GPUREG_FOG_LUT_INDEX, 0);
			GPUCMD_AddWrites(GPUREG_FOG_LUT_DATA0, fogLutData, 128);
		}
		break;
	}
	case 3: for (int i = 0; i < 6; i++) { C3D_TexEnv t = s->texEnv[i]; C3Di_TexEnvBind(i, &t); } break;
	case 4:
		GPUCMD_AddMaskedWrite(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, s->texEnvBuf);
		GPUCMD_AddWrite(GPUREG_TEXENV_BUFFER_COLOR, s->texEnvBufClr);
		GPUCMD_AddWrite(GPUREG_FOG_COLOR, s->fogClr);
		break;
	case 5:
	{
		u32 bound = 0;
		for (int i = 0; i < 3; i++)
			if (o->texBound[i]) { C3D_Tex t = o->tex[i]; C3Di_SetTex(i, &t); bound |= BIT(i); }
		const u32 cfg = (o->texConfig & ~7) | bound | BIT(16);
		GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0xB, cfg);
		GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
		GPUCMD_AddWrite(GPUREG_TEXUNIT0_SHADOW, o->texShadow);
		break;
	}
	case 6: { u32 sc[3]; memcpy(sc, o->scissor, sizeof sc); GPUCMD_AddIncrementalWrites(GPUREG_SCISSORTEST_MODE, sc, 3); break; }
	case 7:
	{
		const u32 pa = osConvertVirtToPhys(indices), base = o->bufInfo.base_paddr;
		if (pa < base) { ok = false; break; }
		GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x1, units);
		drawBody(count, pa, base);
		break;
	}
	default: ok = false;
	}
	u32 n = gpuCmdBufOffset;
	GPUCMD_SetBuffer(oldBuf, oldSize, oldOffset);
	return ok ? n : 0;
}

// Puts a group's state into the context (no dirty flags: its commands were just sent). texSlots[i]: where unit i's texture copy lives
// (the context keeps a pointer); fogLut: the table object the context should point at when fog is on.
void C3Dx_Apply(const void* shared, const void* own, int group, C3D_Tex* const texSlots[3])
{
	const C3Dx_Shared* s = (const C3Dx_Shared*)shared;
	const C3Dx_Own* o = (const C3Dx_Own*)own;
	C3D_Context* ctx = C3Di_GetContext();
	switch (group)
	{
	case 0: ctx->attrInfo = s->attrInfo; break;
	case 1: ctx->bufInfo = o->bufInfo; break;
	case 2: ctx->effect = s->effect; if ((s->texEnvBuf & 7) != GPU_NO_FOG) ctx->fogLut = s->fogLut; break;
	case 3: memcpy(ctx->texEnv, s->texEnv, sizeof s->texEnv); break;
	case 4: ctx->texEnvBuf = s->texEnvBuf; ctx->texEnvBufClr = s->texEnvBufClr; ctx->fogClr = s->fogClr; break;
	case 5:
	{
		u32 bound = 0;
		for (int i = 0; i < 3; i++)
		{
			if (o->texBound[i]) { *texSlots[i] = o->tex[i]; ctx->tex[i] = texSlots[i]; bound |= BIT(i); }
			else ctx->tex[i] = NULL;
		}
		ctx->texConfig = (o->texConfig & ~7) | bound; ctx->texShadow = o->texShadow;
		break;
	}
	case 6: memcpy(ctx->scissor, o->scissor, sizeof o->scissor); break;
	case 7: ctx->flags |= C3DiF_DrawUsed; break;
	}
}

// Appends n pre-built words to the live command buffer. False when they do not fit.
bool C3Dx_Append(const u32* words, u32 n)
{
	if (!gpuCmdBuf || (gpuCmdBufOffset & 1) || gpuCmdBufOffset + n > gpuCmdBufSize) return false;
	memcpy(gpuCmdBuf + gpuCmdBufOffset, words, n * 4);
	gpuCmdBufOffset += n;
	return true;
}

// (Claude v119) the program is being freed: the context must not keep it (C3D_BindProgram reads the old program's shaders to decide what
// to upload; a freed one crashed the emulator). The next bind then uploads the whole program, as after C3D_Init.
void C3Dx_ForgetProgram(const shaderProgram_s* p)
{
	C3D_Context* ctx = C3Di_GetContext();
	if (ctx->program == p) { ctx->program = NULL; ctx->flags &= ~(u32)(C3DiF_Program | C3DiF_VshCode | C3DiF_GshCode); }
}

// Pure relocation query: validates that the index address is representable for
// this captured buffer base. No context mutation or GPU submission.
bool C3Dx_IndexOffset(const void* own,const void* indices,u32* offset)
{
 const C3Dx_Own* o=(const C3Dx_Own*)own;
 const u32 pa=osConvertVirtToPhys(indices),base=o->bufInfo.base_paddr;
 if(pa<base || pa-base>0x7fffffff)return false;
 *offset=pa-base;return true;
}
