#include <string.h>
#include "internal.h"
#include <c3d/uniforms.h>

C3D_FVec C3D_FVUnif[2][C3D_FVUNIF_COUNT];
C3D_IVec C3D_IVUnif[2][C3D_IVUNIF_COUNT];
u16      C3D_BoolUnifs[2];

bool C3D_FVUnifDirty[2][C3D_FVUNIF_COUNT];
bool C3D_IVUnifDirty[2][C3D_IVUNIF_COUNT];
bool C3D_BoolUnifsDirty[2];

static struct
{
	bool dirty;
	int count;
	float24Uniform_s* data;
} C3Di_ShaderFVecData[2];

static bool C3Di_FVUnifEverDirty[2][C3D_FVUNIF_COUNT];
static bool C3Di_IVUnifEverDirty[2][C3D_IVUNIF_COUNT];

void C3D_UpdateUniforms(GPU_SHADER_TYPE type)
{
	int offset = type == GPU_GEOMETRY_SHADER ? (GPUREG_GSH_BOOLUNIFORM-GPUREG_VSH_BOOLUNIFORM) : 0;
	int i = 0;

	// Update FVec uniforms that come from shader constants
	if (C3Di_ShaderFVecData[type].dirty)
	{
		while (i < C3Di_ShaderFVecData[type].count)
		{
			float24Uniform_s* u = &C3Di_ShaderFVecData[type].data[i++];
			GPUCMD_AddIncrementalWrites(GPUREG_VSH_FLOATUNIFORM_CONFIG+offset, (u32*)u, 4);
			C3D_FVUnifDirty[type][u->id] = false;
		}
		C3Di_ShaderFVecData[type].dirty = false;
		i = 0;
	}

	// Update FVec uniforms
	while (i < C3D_FVUNIF_COUNT)
	{
		if (!C3D_FVUnifDirty[type][i])
		{
			// (Windfall) skip four clean flags at a time; same uploads as the one-by-one scan
			if (!(i & 3) && i + 4 <= C3D_FVUNIF_COUNT)
			{
				u32 w;
				memcpy(&w, &C3D_FVUnifDirty[type][i], 4);
				if (!w) { i += 4; continue; }
			}
			i ++;
			continue;
		}

		// Find the number of consecutive dirty uniforms
		int j;
		for (j = i; j < C3D_FVUNIF_COUNT && C3D_FVUnifDirty[type][j]; j ++);

		// Upload the uniforms
		GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG+offset, 0x80000000|i);
		GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA+offset, (u32*)&C3D_FVUnif[type][i], (j-i)*4);

		// Clear the dirty flag
		int k;
		for (k = i; k < j; k ++)
		{
			C3D_FVUnifDirty[type][k] = false;
			C3Di_FVUnifEverDirty[type][k] = true;
		}

		// Advance
		i = j;
	}

	// Update IVec uniforms
	for (i = 0; i < C3D_IVUNIF_COUNT; i ++)
	{
		if (!C3D_IVUnifDirty[type][i]) continue;

		GPUCMD_AddWrite(GPUREG_VSH_INTUNIFORM_I0+offset+i, C3D_IVUnif[type][i]);
		C3D_IVUnifDirty[type][i] = false;
		C3Di_IVUnifEverDirty[type][i] = false;
	}

	// Update bool uniforms
	if (C3D_BoolUnifsDirty[type])
	{
		GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM+offset, 0x7FFF0000 | C3D_BoolUnifs[type]);
		C3D_BoolUnifsDirty[type] = false;
	}
}

void C3Di_DirtyUniforms(GPU_SHADER_TYPE type)
{
	int i;
	C3D_BoolUnifsDirty[type] = true;
	if (C3Di_ShaderFVecData[type].count)
		C3Di_ShaderFVecData[type].dirty = true;
	for (i = 0; i < C3D_FVUNIF_COUNT; i ++)
		C3D_FVUnifDirty[type][i] = C3D_FVUnifDirty[type][i] || C3Di_FVUnifEverDirty[type][i];
	for (i = 0; i < C3D_IVUNIF_COUNT; i ++)
		C3D_IVUnifDirty[type][i] = C3D_IVUnifDirty[type][i] || C3Di_IVUnifEverDirty[type][i];
}

void C3Di_LoadShaderUniforms(shaderInstance_s* si)
{
	GPU_SHADER_TYPE type = si->dvle->type;
	if (si->boolUniformMask)
	{
		C3D_BoolUnifs[type] &= ~si->boolUniformMask;
		C3D_BoolUnifs[type] |= si->boolUniforms;
	}

	if (type == GPU_GEOMETRY_SHADER)
		C3D_BoolUnifs[type] &= ~BIT(15);
	C3D_BoolUnifsDirty[type] = true;

	if (si->intUniformMask)
	{
		int i;
		for (i = 0; i < 4; i ++)
		{
			if (si->intUniformMask & BIT(i))
			{
				C3D_IVUnif[type][i] = si->intUniforms[i];
				C3D_IVUnifDirty[type][i] = true;
			}
		}
	}
	C3Di_ShaderFVecData[type].dirty = true;
	C3Di_ShaderFVecData[type].count = si->numFloat24Uniforms;
	C3Di_ShaderFVecData[type].data = si->float24Uniforms;
}

void C3Di_ClearShaderUniforms(GPU_SHADER_TYPE type)
{
	C3Di_ShaderFVecData[type].dirty = false;
	C3Di_ShaderFVecData[type].count = 0;
	C3Di_ShaderFVecData[type].data = NULL;
}

// (Claude v119) recorded draws: n vertex-shader float uniform rows (ids ascending, rows as x,y,z,w) sent straight to the command buffer,
// consecutive ids in one write, and stored in C3D_FVUnif as if C3D_FVUnifSet + C3D_UpdateUniforms had sent them (clean, ever dirty).
void C3Dx_Uniforms(const u8* ids, const float (*rows)[4], int n)
{
	int i = 0;
	while (i < n)
	{
		int j = i + 1;
		while (j < n && ids[j] == ids[j - 1] + 1) j++;
		u32 data[4 * 96];
		for (int k = i; k < j; k++)
		{
			C3D_FVec* v = &C3D_FVUnif[GPU_VERTEX_SHADER][ids[k]];
			v->x = rows[k][0]; v->y = rows[k][1]; v->z = rows[k][2]; v->w = rows[k][3];
			memcpy(&data[(k - i) * 4], v, 16);
			C3D_FVUnifDirty[GPU_VERTEX_SHADER][ids[k]] = false;
			C3Di_FVUnifEverDirty[GPU_VERTEX_SHADER][ids[k]] = true;
		}
		GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000 | ids[i]);
		GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, data, (j - i) * 4);
		i = j;
	}
}

// Command templates already emitted these rows. Keep exactly the same CPU
// shadow history as C3Dx_Uniforms, without emitting a second uniform upload.
void C3Dx_UniformsCommit(const u8* ids, const float (*rows)[4], int n)
{
 for(int i=0;i<n;++i){
  C3D_FVec* v=&C3D_FVUnif[GPU_VERTEX_SHADER][ids[i]];
  v->x=rows[i][0];v->y=rows[i][1];v->z=rows[i][2];v->w=rows[i][3];
  C3D_FVUnifDirty[GPU_VERTEX_SHADER][ids[i]]=false;
  C3Di_FVUnifEverDirty[GPU_VERTEX_SHADER][ids[i]]=true;
 }
}
