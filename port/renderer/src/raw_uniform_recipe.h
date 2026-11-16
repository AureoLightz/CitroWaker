#pragma once
#include "uniform_recipe.h"
#include <cstddef>
namespace t2p {
// Direct-event input packing. An immutable validated recipe reads the live XF
// register words and writes only the shader's consumed rows. No GxState copy,
// VtxUniforms/CompactUniforms creation, shader lookup or PackedUniforms exists
// in this operation. Projection mapping and texture scale are supplied by the
// owner, because rendered-copy and viewport policy belong to the render stage.
inline bool rawRecipeValid(const UniformRecipe& recipe){
 if(recipe.destinationRows>96||recipe.poseCount>10)return false;
 for(const auto& o:recipe.ops){
  if(o.destination>=recipe.destinationRows||o.source>UniformRecipe::Scale)return false;
  switch(o.source){
   case UniformRecipe::Projection:if(o.row>=4||o.components!=4)return false;break;
   case UniformRecipe::Position:case UniformRecipe::Normal:case UniformRecipe::Texture:if(o.row>=3||o.index>=10||o.components!=4)return false;break;
   case UniformRecipe::Scale:if(o.index>=3||o.components!=2)return false;break;
   case UniformRecipe::Material:case UniformRecipe::Ambient:case UniformRecipe::LightColor:if(o.components!=4)return false;[[fallthrough]];
   case UniformRecipe::LightPos:case UniformRecipe::LightDir:case UniformRecipe::LightCos:case UniformRecipe::LightDist:if(o.index>=8||(o.source!=UniformRecipe::Material&&o.source!=UniformRecipe::Ambient&&o.source!=UniformRecipe::LightColor&&o.components!=3))return false;break;
   default:break;
  }
 }
 return true;
}
inline bool applyRawRecipe(const UniformRecipe& recipe,PVec* output,size_t rows,const uint32_t* xf,size_t words,int selected,const float projection[4][4],const float scale[3][2]){
 // Recipe must have passed rawRecipeValid at creation/load, not per draw.
 if(!output||rows<recipe.destinationRows||!xf||words<0x1100||selected<0||selected>=10||!projection||!scale)return false;
 auto color=[](uint32_t x,float* out){out[0]=float(x>>24);out[1]=float((x>>16)&255);out[2]=float((x>>8)&255);out[3]=float(x&255);};
 for(const auto& o:recipe.ops){float* dest=output[o.destination].v;const uint32_t* source=nullptr;size_t n=4;
  switch(o.source){
   case UniformRecipe::Zero:std::memset(dest,0,16);continue;
   case UniformRecipe::Projection:std::memcpy(dest,projection[o.row],16);continue;
   case UniformRecipe::Position:{const int slot=o.index==0?selected:o.index;source=xf+slot*12+o.row*4;break;}
   case UniformRecipe::Normal:{const int slot=o.index==0?selected:o.index;source=xf+0x400+slot*9+o.row*3;n=3;break;}
   case UniformRecipe::Texture:source=xf+120+o.index*12+o.row*4;break;
   case UniformRecipe::Material:color(xf[0x100c],dest);continue;
   case UniformRecipe::Ambient:color(xf[0x100a],dest);continue;
   case UniformRecipe::LightColor:color(xf[0x600+o.index*16+3],dest);continue;
   case UniformRecipe::LightPos:source=xf+0x600+o.index*16+10;n=3;break;
   case UniformRecipe::LightDir:source=xf+0x600+o.index*16+13;n=3;break;
   case UniformRecipe::LightCos:source=xf+0x600+o.index*16+4;n=3;break;
   case UniformRecipe::LightDist:source=xf+0x600+o.index*16+7;n=3;break;
   case UniformRecipe::Scale:std::memset(dest,0,16);std::memcpy(dest,scale[o.index],8);continue;
   default:return false; // unreachable for a validated immutable recipe
  }
  if(n<4)dest[3]=0;std::memcpy(dest,source,n*4);
 }
 return true;
}
}
