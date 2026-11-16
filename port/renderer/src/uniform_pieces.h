#pragma once
#include "vtx.h"
#include "gxfifo.h"
#include <array>
#include <memory>
#include <cstring>
namespace t2p {
// Immutable independent blocks: changing a colour or projection must not rebuild
// every light, texture matrix and all ten poses. All words remain exact.
struct UniformCameraPiece {float proj[4][4];};
struct UniformColorPiece {float mat[4],amb[4];};
struct UniformLightPiece {VtxLight light[8];};
struct UniformTexturePiece {float matrix[10][3][4],scale[3][2];};
struct UniformPieces {
 std::shared_ptr<const UniformCameraPiece> camera;
 std::shared_ptr<const UniformColorPiece> color;
 std::shared_ptr<const UniformLightPiece> lights;
 std::shared_ptr<const UniformTexturePiece> texture;
 VtxUniformView view(const float (*pos)[3][4],const float (*nrm)[3][4],int n)const{return {camera->proj,pos,nrm,color->mat,color->amb,lights->light,texture->matrix,texture->scale,n};}
};
template<class T>struct UniformPieceMemo {
 std::array<std::shared_ptr<const T>,8> entries{};unsigned next=0;
 std::shared_ptr<const T> find(const T& wanted,const std::shared_ptr<const T>& previous={}){
  if(previous&&!std::memcmp(previous.get(),&wanted,sizeof(T)))return previous;
  for(const auto& entry:entries)if(entry&&!std::memcmp(entry.get(),&wanted,sizeof(T)))return entry;
  auto entry=std::make_shared<const T>(wanted);entries[next++&7]=entry;return entry;
 }
};
extern bool g_uniformPieces;
inline std::shared_ptr<const UniformPieces> makeUniformPieces(const uint32_t* xf,const float projection[4][4],uint32_t dirty,const std::shared_ptr<const UniformPieces>& previous){
 static thread_local UniformPieceMemo<UniformCameraPiece> cameras;
 static thread_local UniformPieceMemo<UniformColorPiece> colors;
 static thread_local UniformPieceMemo<UniformLightPiece> lights;
 static thread_local UniformPieceMemo<UniformTexturePiece> textures;
 UniformPieces result;
 if(previous&&!(dirty&kStateDirtyProj))result.camera=previous->camera;
 else{UniformCameraPiece wanted;std::memcpy(wanted.proj,projection,sizeof wanted.proj);result.camera=cameras.find(wanted,previous?previous->camera:nullptr);}
 auto unpack=[](uint32_t v,float* out){out[0]=float(v>>24);out[1]=float((v>>16)&255);out[2]=float((v>>8)&255);out[3]=float(v&255);};
 if(previous&&!(dirty&kStateDirtyChan))result.color=previous->color;
 else{UniformColorPiece wanted;unpack(xf[0x100c],wanted.mat);unpack(xf[0x100a],wanted.amb);result.color=colors.find(wanted,previous?previous->color:nullptr);}
 if(previous&&!(dirty&kStateDirtyLight))result.lights=previous->lights;
 else{UniformLightPiece wanted;for(unsigned i=0;i<8;++i){auto& light=wanted.light[i];const auto* source=xf+0x600+i*16;unpack(source[3],light.color);std::memcpy(light.cosatt,source+4,12);std::memcpy(light.distatt,source+7,12);std::memcpy(light.pos,source+10,12);std::memcpy(light.dir,source+13,12);}result.lights=lights.find(wanted,previous?previous->lights:nullptr);}
 if(previous&&(!(dirty&kStateDirtyXfMtx)||!std::memcmp(previous->texture->matrix,xf+120,sizeof previous->texture->matrix)))result.texture=previous->texture;
 else{UniformTexturePiece wanted;std::memcpy(wanted.matrix,xf+120,sizeof wanted.matrix);for(auto& scale:wanted.scale)scale[0]=scale[1]=1;result.texture=textures.find(wanted,previous?previous->texture:nullptr);}
 if(previous&&result.camera==previous->camera&&result.color==previous->color&&result.lights==previous->lights&&result.texture==previous->texture)return previous;
 return std::make_shared<const UniformPieces>(std::move(result));
}
}
