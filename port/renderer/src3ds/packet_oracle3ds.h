#pragma once
#include "submit.h"
#include <cstring>
#include <tuple>
namespace t2p {
// The two mutable execution memos have copy constructors that start empty.
// Clone the pass chain so the reference builder never races the worker's TEV
// constant memo on a shared compiled material.
inline std::shared_ptr<const PicaDraw> packetOracleProgram(const PicaDraw& d){
 auto p=std::make_shared<PicaDraw>(d);if(d.pass2)p->pass2=packetOracleProgram(*d.pass2);return p;
}

inline thread_local int packetDiffLine=0;
inline bool packetDifference(int line){packetDiffLine=line;return false;}
inline bool packetBytesEqual(const void* a,const void* b,size_t n){return !n||!std::memcmp(a,b,n);}
inline bool packetFloatEqual(float a,float b){return packetBytesEqual(&a,&b,4);}
inline bool samePreparedPacket(const DrawPacket& a,const DrawPacket& b){
 if(a.ok!=b.ok||a.notes!=b.notes)return packetDifference(__LINE__);
 if(!a.ok)return true;
 if(a.stride!=b.stride||a.vertexCount!=b.vertexCount||a.vertexBytes()!=b.vertexBytes()||a.indexCount()!=b.indexCount()||a.layout.size()!=b.layout.size()||a.texenv.size()!=b.texenv.size()||a.texenvUpdateBuffer!=b.texenvUpdateBuffer||a.usesPackedUniforms!=b.usesPackedUniforms)return packetDifference(__LINE__);
 if(!packetBytesEqual(a.vertexData(),b.vertexData(),a.vertexBytes())||!packetBytesEqual(a.indexData(),b.indexData(),a.indexCount()*2)||!packetBytesEqual(a.texenv.data(),b.texenv.data(),a.texenv.size()*sizeof(HwStage)))return packetDifference(__LINE__);
 for(size_t i=0;i<a.layout.size();++i){const auto& x=a.layout[i];const auto& y=b.layout[i];if(std::tie(x.kind,x.index,x.reg,x.components,x.ubyte,x.offset)!=std::tie(y.kind,y.index,y.reg,y.components,y.ubyte,y.offset))return packetDifference(__LINE__);}
 if(a.usesPackedUniforms){if(!a.packedUniforms||!b.packedUniforms)return packetDifference(__LINE__);for(int i=0;i<8;++i){const auto& x=a.packedUniforms->arrays[i];const auto& y=b.packedUniforms->arrays[i];if(x.count!=y.count||x.count<0||x.count>40||!packetBytesEqual(x.rows,y.rows,size_t(x.count)*sizeof(PVec)))return packetDifference(__LINE__);}}
 else{if(a.uniforms.size()!=b.uniforms.size())return packetDifference(__LINE__);for(const auto& kv:a.uniforms){auto it=b.uniforms.find(kv.first);if(it==b.uniforms.end()||kv.second.size()!=it->second.size()||!packetBytesEqual(kv.second.data(),it->second.data(),kv.second.size()*sizeof(PVec)))return packetDifference(__LINE__);}}
 if(std::tie(a.colorUpdate,a.alphaUpdate,a.dither,a.dstAlphaEnable,a.dstAlpha,a.pixelFmt,a.zCompBeforeTex,a.cullMode)!=std::tie(b.colorUpdate,b.alphaUpdate,b.dither,b.dstAlphaEnable,b.dstAlpha,b.pixelFmt,b.zCompBeforeTex,b.cullMode))return packetDifference(__LINE__);
 const auto& f=a.frame;const auto& g=b.frame;
 if(f.ok!=g.ok||f.mirrored!=g.mirrored||f.picaCull!=g.picaCull||!packetBytesEqual(f.proj,g.proj,sizeof f.proj)||!packetBytesEqual(f.post,g.post,sizeof f.post))return packetDifference(__LINE__);
 if(std::tie(f.scissorWindow.x0,f.scissorWindow.y0,f.scissorWindow.x1,f.scissorWindow.y1,f.scissorLogical.x0,f.scissorLogical.y0,f.scissorLogical.x1,f.scissorLogical.y1)!=std::tie(g.scissorWindow.x0,g.scissorWindow.y0,g.scissorWindow.x1,g.scissorWindow.y1,g.scissorLogical.x0,g.scissorLogical.y0,g.scissorLogical.x1,g.scissorLogical.y1))return packetDifference(__LINE__);
 if(f.depth.zBuffer!=g.depth.zBuffer||f.depth.clear!=g.depth.clear||!packetFloatEqual(f.depth.scale,g.depth.scale)||!packetFloatEqual(f.depth.offset,g.depth.offset))return packetDifference(__LINE__);
 for(int i=0;i<3;++i){const auto& u=a.units[i];const auto& v=b.units[i];const auto& x=u.tex;const auto& y=v.tex;
  if(std::tie(u.texmap,u.texcoord,u.scratch,u.needAlpha)!=std::tie(v.texmap,v.texcoord,v.scratch,v.needAlpha)||!packetFloatEqual(u.scaleS,v.scaleS)||!packetFloatEqual(u.scaleT,v.scaleT))return packetDifference(__LINE__);
  if(std::tie(x.valid,x.addr,x.width,x.height,x.fmt,x.wrapS,x.wrapT,x.magLinear,x.minLinear,x.mipMode,x.edgeLod,x.biasClamp,x.aniso,x.tlutOffset,x.tlutFmt)!=std::tie(y.valid,y.addr,y.width,y.height,y.fmt,y.wrapS,y.wrapT,y.magLinear,y.minLinear,y.mipMode,y.edgeLod,y.biasClamp,y.aniso,y.tlutOffset,y.tlutFmt)||!packetFloatEqual(x.lodBias,y.lodBias)||!packetFloatEqual(x.minLod,y.minLod)||!packetFloatEqual(x.maxLod,y.maxLod))return packetDifference(__LINE__);
 }
 if(bool(a.second)!=bool(b.second))return packetDifference(__LINE__);
 return !a.second||samePreparedPacket(*a.second,*b.second);
}
}
