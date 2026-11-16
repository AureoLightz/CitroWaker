#!/usr/bin/env python3
"""Generate 3DS SDK overrides; never modify fetched Aurora or game sources."""
from pathlib import Path
import re,sys
lib=Path(sys.argv[1]);out=Path(sys.argv[2]);sw=lib.parents[3]
# lib = SwitchWaker/build/native-linux/_deps/aurora-src/lib
sw=lib
while sw.name!='SwitchWaker' and sw.parent!=sw:sw=sw.parent
if sw.name!='SwitchWaker':raise SystemExit('cannot locate SwitchWaker root')
sdk=lib/'dolphin/gx';out.mkdir(parents=True,exist_ok=True)
def save(p,s):
 if not p.exists() or p.read_text()!=s:p.write_text(s)
def includes(s,source):
 def one(m):
  name=m[1]
  if name=='__gx.h':return '#include "/arm/gen/typed_gx/__gx.h"'
  p=(source.parent/name).resolve()
  if not p.exists(): return m[0] # public SDK include, resolved by original -I flags
  return '#include "/work/'+str(p.relative_to(sw.resolve()))+'"'
 return re.sub(r'^#include "([^"]+)"',one,s,flags=re.M)
header=includes((sdk/'__gx.h').read_text(),sdk/'__gx.h')
header+='\n#include "/arm/src/typed_gx_emit_3ds.h"\n'
save(out/'__gx.h',header)
def edit_func(s,name,fn):
 start=s.index('void '+name+'(');end=s.find('\nvoid ',start+1)
 if end<0:end=len(s)
 return s[:start]+fn(s[start:end])+s[end:]
for p in sdk.glob('*.cpp'):
 s=includes(p.read_text(),p)
 if p.name=='GXDispList.cpp':
  s += r'''
#include "/work/game/include/JSystem/J3DGraphBase/J3DDirect3ds.h"
#include "/hw6/src/gxstateprogram.h"
#include <cstdlib>
extern "C" int n3dsJ3dGeometryOn(void);
extern "C" volatile unsigned g_n3dsShapeGen;
extern "C" void n3dsShapePageMark(const void*);
extern "C" int n3dsJ3dProgramsOn(void);
extern "C" unsigned g_n3dsModelRecords;
extern "C" unsigned g_n3dsMaterialRecords;
extern "C" unsigned g_n3dsMaterialIdentity;
extern "C" uint64_t n3dsMaterialRegisterVersioned(const uint8_t*,uint32_t);
namespace {
struct OwnedPoseOp {u16 address,count;u32 words[12];};
struct OwnedPoseCapture {bool active=false,failed=false;unsigned count=0;OwnedPoseOp ops[24];};
thread_local OwnedPoseCapture sPose;
}
extern "C" int n3dsJ3dPoseBegin(void){
 static const bool capturing=std::getenv("COS_WWRP")!=nullptr;
 if(!g_n3dsModelRecords||capturing||aurora::gx::fifo::in_display_list()||sPose.active)return 0;
 sPose.active=true;sPose.failed=false;sPose.count=0;return 1;
}
extern "C" int n3dsJ3dPoseWrite(unsigned addr,const void* source,unsigned rows,unsigned cols,unsigned stride){
 if(!sPose.active)return 0;
 if(!source||rows!=3||(cols!=3&&cols!=4)||stride<cols||sPose.count==24||addr+rows*cols>0x600){sPose.failed=true;return 1;}
 auto& op=sPose.ops[sPose.count++];op.address=u16(addr);op.count=u16(rows*cols);
 const auto* bytes=static_cast<const uint8_t*>(source);
 for(unsigned r=0;r<rows;++r)std::memcpy(op.words+r*cols,bytes+r*stride*4,cols*4);
 return 1;
}
extern "C" int n3dsJ3dPoseEnd(void){
 if(!sPose.active)return 0;sPose.active=false;
 if(sPose.failed||!sPose.count)return 0;
 // Preserve stream order: this owns the original matrix words BEFORE the
 // existing SDK dirty-state/flush-primitive path and shape reference execute.
 aurora::gx::fifo::write_u8(0x50);aurora::gx::fifo::write_u16(0x7B);
 aurora::gx::fifo::write_u16(u16(sPose.count));
 for(unsigned i=0;i<sPose.count;++i){const auto& op=sPose.ops[i];aurora::gx::fifo::write_u16(op.address);aurora::gx::fifo::write_u16(op.count);aurora::gx::fifo::write_data(op.words,op.count*4);}
 aurora::gx::fifo::publish();return 1;
}
extern "C" int n3dsJ3dTryGroupV2(const N3dsJ3dGroupV2* o){
  static const bool capturing=std::getenv("COS_WWRP")!=nullptr;
  // Dirty SDK state used to be flushed AFTER the matrix loads. Refuse rather
  // than moving those writes across the model record and changing its inputs.
  if(!g_n3dsModelRecords||capturing||!o||o->version!=2||aurora::gx::fifo::in_display_list()||__gx->dirtyState||*reinterpret_cast<u32*>(&__gx->vNum))return 0;
  const auto* p=static_cast<const uint8_t*>(o->displayList);
  if(!n3dsJ3dGeometryOn()||!p||!o->displayListBytes||o->displayListBytes>262144||!o->matrixCount||o->matrixCount>10)return 0;
  n3dsShapePageMark(p);
  aurora::gx::fifo::write_u8(0x50);aurora::gx::fifo::write_u16(0x7C);
  aurora::gx::fifo::write_u64(uint64_t(reinterpret_cast<uintptr_t>(p)));
  aurora::gx::fifo::write_u32(o->displayListBytes);aurora::gx::fifo::write_u32(g_n3dsShapeGen);
  aurora::gx::fifo::write_u8(uint8_t(o->matrixCount));
  for(u32 i=0;i<o->matrixCount;++i)aurora::gx::fifo::write_u16(o->matrixIndices[i]);
  aurora::gx::fifo::publish();return 1;
}
extern "C" uint64_t n3dsMaterialRegister(const uint8_t*,uint32_t);
extern "C" int n3dsJ3dTryDirectV1(const N3dsJ3dOfferV1* o) {
  static const bool capturing = std::getenv("COS_WWRP") != nullptr;
  if(capturing || !o || o->version!=1 || aurora::gx::fifo::in_display_list())return 0;
  const bool shape=o->kind==1;
  if(shape ? !n3dsJ3dGeometryOn() : ((!n3dsJ3dProgramsOn() && !g_n3dsMaterialRecords && !g_n3dsMaterialIdentity) || (o->kind!=2 && o->kind!=3)))return 0;
  const auto* p=static_cast<const uint8_t*>(o->displayList);
  if(shape){
    // (Claude v95) shape by reference (GX_AURORA 0x7D: u64 address, u32 size, u32 model generation): only lists drawn from the model's own
    // immutable arrays (producer flag 0x80000000); the list is not copied into the FIFO and the renderer caches its decode by address
    if(!(o->flags&0x80000000u) || !p || !o->displayListBytes || o->displayListBytes>262144)return 0;
    if(__gx->dirtyState!=0)__GXSetDirtyState();
    if(*reinterpret_cast<u32*>(&__gx->vNum)!=0)__GXSendFlushPrim();
    n3dsShapePageMark(p);   // a release of this page (JKRHeap.cpp) starts a new model generation
    aurora::gx::fifo::write_u8(0x50);aurora::gx::fifo::write_u16(0x7D);
    aurora::gx::fifo::write_u64(uint64_t(reinterpret_cast<uintptr_t>(p)));
    aurora::gx::fifo::write_u32(o->displayListBytes);aurora::gx::fifo::write_u32(g_n3dsShapeGen);
    aurora::gx::fifo::publish();return 1;
  }
  if(g_n3dsMaterialRecords||g_n3dsMaterialIdentity){
    // Register before any SDK/FIFO effect, so refusal retains the original path.
    if(g_n3dsMaterialIdentity&&p&&o->displayListBytes&&o->displayListBytes<=2048){n3dsShapePageMark(p);n3dsShapePageMark(p+o->displayListBytes-1);}
    const uint64_t token=g_n3dsMaterialIdentity?n3dsMaterialRegisterVersioned(p,o->displayListBytes):n3dsMaterialRegister(p,o->displayListBytes);
    if(token){
      if(__gx->dirtyState!=0)__GXSetDirtyState();
      if(*reinterpret_cast<u32*>(&__gx->vNum)!=0)__GXSendFlushPrim();
      aurora::gx::fifo::write_u8(0x50);aurora::gx::fifo::write_u16(0x7A);
      aurora::gx::fifo::write_u64(token);aurora::gx::fifo::publish();return 1;
    }
    if(!n3dsJ3dProgramsOn())return 0;
  }
  if(!t2p::GxStateProgram::valid(p,o->displayListBytes))return 0;
  if(__gx->dirtyState!=0)__GXSetDirtyState();
  if(*reinterpret_cast<u32*>(&__gx->vNum)!=0)__GXSendFlushPrim();
  aurora::gx::fifo::write_u8(0x50);aurora::gx::fifo::write_u16(shape?0x7F:0x7E);
  if(shape)aurora::gx::fifo::write_u64(uint64_t(reinterpret_cast<uintptr_t>(o->owner)));
  aurora::gx::fifo::write_u32(o->displayListBytes);
  aurora::gx::fifo::write_data(p,o->displayListBytes);
  aurora::gx::fifo::publish();return 1;
}
'''
 if p.name=='GXVert.cpp':
  assert '  sInBegin = true;' in s and '    sInBegin = false;' in s
  s=s.replace('  sInBegin = true;','  sInBegin = true;\n  g_n3dsTypedPrimitive = 1;')
  s=s.replace('    sInBegin = false;','    sInBegin = false;\n    g_n3dsTypedPrimitive = 0;')
 if p.name=='GXTransform.cpp':
  for name,call in (
    ('GXLoadPosMtxImm','n3ds_typed::xfRows(id * 4, mtx, 3, 4, 4)'),
    ('GXLoadNrmMtxImm','n3ds_typed::xfRows(id * 3 + 0x400, mtx, 3, 3, 4)'),
    ('GXLoadTexMtxImm','n3ds_typed::xfRows(addr, reinterpret_cast<const f32*>(mtx_), count / 4, 4, 4)')):
   def one(body,call=call):
    needle='  GX_WRITE_U8(0x10);';assert needle in body
    return body.replace(needle,'  if ('+call+') return;\n'+needle,1)
   s=edit_func(s,name,one)
  for name in ('GXSetProjection','GXSetProjectionv'):
   def projection(body):
    needle='  GX_WRITE_U8(0x10);';assert needle in body
    add='''  if (n3ds_typed::allowed()) {
    u32 words[7];
    std::memcpy(words, __gx->projMtx, 24);
    words[6] = __gx->projType;
    if (n3ds_typed::xfWords(0x1020, words, 7)) { __gx->bpSent = 0; return; }
  }
'''
    return body.replace(needle,add+needle,1)
   s=edit_func(s,name,projection)
 save(out/p.name,s)
# Existing generated framebuffer omits WebGPU code. Keep its generation owned
# by build.sh; only redirect its __gx include before the normal compare/move.
frame=out.parent/'GXFrameBuffer_3ds.cpp.new'
if frame.exists():
 s=frame.read_text().replace('#include "__gx.h"','#include "/arm/gen/typed_gx/__gx.h"')
 frame.write_text(s)

# Generated FIFO watermark: exact processed offset, reset with every buffer reset.
# This avoids a 64-bit atomic publication check for each direct state write.
fifo=out.parent/"fifo_3ds.cpp.new"
if fifo.exists():
 s=fifo.read_text()
 s='#include "/arm/src/typed_gx_api_3ds.h"\n'+s
 needle="    sProcessed.store(processed, order);"
 assert needle in s
 s=s.replace(needle,needle+"\n    if (kProcessingMode == ProcessingMode::Inline) g_n3dsTypedConsumedSize = uint32_t(processed - sStreamBase);")
 assert "detail::sBufferSize = 0;" in s
 s=s.replace("detail::sBufferSize = 0;","detail::sBufferSize = 0; if (kProcessingMode == ProcessingMode::Inline) g_n3dsTypedConsumedSize = 0;")
 fifo.write_text(s)
