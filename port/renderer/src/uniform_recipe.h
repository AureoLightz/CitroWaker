#pragma once
#include "vtx.h"
#include <algorithm>
#include <cstring>
#include <vector>
namespace t2p {
// A compiled mapping from owned game uniform views to the exact packed rows.
// No temporary PackedUniforms, strings, maps or shader-layout discovery at run time.
struct UniformRecipe {
 enum Source:uint8_t {Zero,Projection,Position,Normal,Material,Ambient,LightColor,LightPos,LightDir,LightCos,LightDist,Texture,Scale};
 struct Range {uint8_t array;uint16_t first,count,destination;};
 struct Op {uint16_t destination;uint8_t source,index,row,components;bool operator==(const Op&)const=default;};
 std::vector<Op> ops;uint16_t destinationRows=0;uint8_t poseCount=0;
 bool operator==(const UniformRecipe& b)const{return destinationRows==b.destinationRows&&poseCount==b.poseCount&&ops==b.ops;}
 bool compile(const VtxSetup& setup,const VtxShader& shader,const std::vector<Range>& ranges){
  if(setup.numPosMtx<1||setup.numPosMtx>10)return false;
  UniformRecipe fresh;
  for(const auto& range:ranges)for(unsigned n=0;n<range.count;++n){
   const unsigned row=range.first+n,dest=range.destination+n;if(dest>=96)return false;
   Op o{};o.destination=uint16_t(dest);o.components=4;
   switch(range.array){
    case 0:if(row>=4)return false;o.source=Projection;o.row=uint8_t(row);break;
    case 1:case 2:if(row>=unsigned(3*setup.numPosMtx)||row>=30)return false;o.source=range.array==1?Position:Normal;o.index=uint8_t(row/3);o.row=uint8_t(row%3);fresh.poseCount=std::max(fresh.poseCount,uint8_t(o.index+1));break;
    case 3:case 4:if(row)return false;o.source=range.array==3?Material:Ambient;break;
    case 5:{if(row>=unsigned(5*std::max(shader.numLights,0)))return false;int light=-1;for(int i=0;i<8;++i)if(shader.lightSlot[i]==int(row/5))light=i;
     if(light<0){o.source=Zero;break;}o.index=uint8_t(light);const Source fields[]={LightColor,LightPos,LightDir,LightCos,LightDist};o.source=fields[row%5];o.components=row%5?3:4;break;}
    case 6:{if(row>=unsigned(3*std::max(shader.numTexMtx,0)))return false;int matrix=-1;bool found=false;for(size_t i=0;i<setup.texgen.size()&&i<3;++i)if(shader.texMtxSlot[i]==int(row/3)){found=true;matrix=(setup.texgen[i].mtx-30)/3;}
     if(!found){o.source=Zero;break;}if(matrix<0||matrix>=10)return false;o.source=Texture;o.index=uint8_t(matrix);o.row=uint8_t(row%3);break;}
    case 7:{if(row>=unsigned(std::max(shader.numTexScale,0)))return false;int unit=-1;for(size_t i=0;i<setup.texgen.size()&&i<3;++i)if(shader.texScaleSlot[i]==int(row))unit=int(i);
     if(unit<0){o.source=Zero;break;}o.source=Scale;o.index=uint8_t(unit);o.components=2;break;}
    default:return false;
   }
   fresh.ops.push_back(o);fresh.destinationRows=std::max(fresh.destinationRows,uint16_t(dest+1));
  }
  std::vector<bool> used(fresh.destinationRows);for(const auto& op:fresh.ops){if(used[op.destination])return false;used[op.destination]=true;}
  *this=std::move(fresh);return true;
 }
 bool apply(PVec* destination,size_t rows,const VtxUniformView& u,const float projection[4][4],const float scale[3][2],const float (*const* positions)[4]=nullptr,const float (*const* normals)[4]=nullptr,size_t mappedCount=0)const{
  if((destinationRows&&!destination)||rows<destinationRows||((positions||normals)?(!positions||!normals||mappedCount<poseCount):u.poseCount<poseCount))return false;
  for(const auto& op:ops){const float* src=nullptr;
   switch(op.source){
    case Zero:break;case Projection:src=projection[op.row];break;case Position:src=positions?positions[op.index][op.row]:u.pos[op.index][op.row];break;case Normal:src=normals?normals[op.index][op.row]:u.nrm[op.index][op.row];break;
    case Material:src=u.matColor;break;case Ambient:src=u.ambColor;break;case LightColor:src=u.light[op.index].color;break;case LightPos:src=u.light[op.index].pos;break;
    case LightDir:src=u.light[op.index].dir;break;case LightCos:src=u.light[op.index].cosatt;break;case LightDist:src=u.light[op.index].distatt;break;
    case Texture:src=u.texMtx[op.index][op.row];break;case Scale:src=scale[op.index];break;
   }
   auto* out=destination[op.destination].v;if(!src||op.components<4)std::memset(out,0,16);if(src)std::memcpy(out,src,size_t(op.components)*4);
  }
  return true;
 }
};
}
