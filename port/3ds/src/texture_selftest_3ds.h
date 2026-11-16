// Standalone diagnostic entry only; never included in the normal player main.
#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "texconv.h"
#include "picaenc.h"
#include "backend3ds.h"
#include "n3ds.h"
extern "C" void C3Di_RenderQueueWaitDone(void);
namespace texturecheck {
struct Vertex { float p[4],tc[4]; };
inline unsigned tiled(unsigned x,unsigned y){unsigned m=0;for(unsigned b=0;b<3;++b)m|=((x>>b)&1)<<(2*b)|((y>>b)&1)<<(2*b+1);return ((y>>3)*8+(x>>3))*64+m;}
inline int run(){
 FILE* report=fopen(N3DS_ROOT "/texture-selftest.txt","w");
 FILE* replay=fopen(N3DS_ROOT "/claude-replay/results.txt","w");
 auto line=[&](const char* msg){if(report){fputs(msg,report);fflush(report);}if(replay){fputs(msg,replay);fflush(replay);}n3dsLog("%s",msg);};
 unsigned errors=0;line("Windfall GPU texture selftest v4 (screen-copy coverage); 13 formats, base and 3 forced mip levels, 3328 exact samples\n");
 if(!C3D_Init(256u<<10)){line("ERROR C3D_Init\n");if(report)fclose(report);if(replay)fclose(replay);return 1;}
 auto* target=C3D_RenderTargetCreate(64,64,GPU_RB_RGBA8,-1);
 const char* src=R"(.constf white(1,1,1,1)
.in p v0
.in tc v1
.out op position
.out oc color
.out ot texcoord0
.proc main
 mov op,p
 mov oc,white
 mov ot,tc
 end
.end
)";
 auto bin=t2p::assemblePica(src,"texturecheck.v.pica");
 DVLB_s* dv=bin.ok?DVLB_ParseFile(reinterpret_cast<u32*>(bin.shbin.data()),bin.shbin.size()):nullptr;
 shaderProgram_s program{};shaderProgramInit(&program);
 bool shaderOK=dv&&R_SUCCEEDED(shaderProgramSetVsh(&program,&dv->DVLE[0]));
 auto* verts=static_cast<Vertex*>(linearAlloc(6*sizeof(Vertex)));
 if(!target||!shaderOK||!verts){line("ERROR selftest setup\n");errors++;}
 else {
  const Vertex quad[6]={{{-1,-1,0,1},{0,1,0,1}},{{1,-1,0,1},{1,1,0,1}},{{1,1,0,1},{1,0,0,1}},{{-1,-1,0,1},{0,1,0,1}},{{1,1,0,1},{1,0,0,1}},{{-1,1,0,1},{0,0,0,1}}};
  memcpy(verts,quad,sizeof quad);GSPGPU_FlushDataCache(verts,sizeof quad);
  std::vector<uint8_t> rgba(8*8*4);for(int y=0;y<8;++y)for(int x=0;x<8;++x){auto* p=rgba.data()+(y*8+x)*4;p[0]=uint8_t(x*37+y*19);p[1]=uint8_t(x*13+y*67);p[2]=uint8_t(x*53+y*31);p[3]=uint8_t((x+y*3)*17);}
  for(int levelCase=-1;levelCase<3;++levelCase) for(int fmt:{0,1,2,3,4,5,7,8,9,10,11,12,13}){
   const int base=levelCase<0?8:32,levels=levelCase<0?1:3;
   std::vector<uint8_t> encoded[3];
   for(int l=0;l<levels;++l){int dim=base>>l;std::vector<uint8_t> src;
    if(levelCase<0)src=rgba;else{src.resize(dim*dim*4);const uint8_t col[3][4]={{17,51,153,255},{68,170,34,170},{221,85,119,85}};for(int i=0;i<dim*dim;++i)memcpy(src.data()+i*4,col[l],4);}
    if(fmt==12||fmt==13)t2p::picaEncodeEtc1(src,dim,dim,fmt==13,0,encoded[l]);else t2p::picaEncode(fmt,src,dim,dim,encoded[l],true,false);
   }
   C3D_TexInitParams params{};params.width=base;params.height=base;params.maxLevel=levels-1;params.format=GPU_TEXCOLOR(fmt);params.type=GPU_TEX_2D;
   C3D_Tex tex{};if(!C3D_TexInitWithParams(&tex,nullptr,params)){++errors;line("ERROR texture alloc\n");continue;}
   for(int l=0;l<levels;++l)C3D_TexLoadImage(&tex,encoded[l].data(),GPU_TEXFACE_2D,l);
   C3D_TexFlush(&tex);C3D_TexSetFilter(&tex,GPU_NEAREST,GPU_NEAREST);C3D_TexSetWrap(&tex,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
   if(levelCase>=0){C3D_TexSetFilterMipmap(&tex,GPU_NEAREST);tex.minLevel=tex.maxLevel=levelCase;tex.lodBias=8*256;}

   C3D_FrameBegin(C3D_FRAME_SYNCDRAW);C3D_RenderTargetClear(target,C3D_CLEAR_COLOR,0,0);C3D_FrameDrawOn(target);C3D_BindProgram(&program);
   auto* ai=C3D_GetAttrInfo();AttrInfo_Init(ai);AttrInfo_AddLoader(ai,0,GPU_FLOAT,4);AttrInfo_AddLoader(ai,1,GPU_FLOAT,4);
   auto* bi=C3D_GetBufInfo();BufInfo_Init(bi);BufInfo_Add(bi,verts,sizeof(Vertex),2,0x10);
   C3D_CullFace(GPU_CULL_NONE);C3D_DepthMap(true,1,1);C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);C3D_AlphaTest(false,GPU_ALWAYS,0);C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
   for(int i=0;i<6;++i)C3D_TexEnvInit(C3D_GetTexEnv(i));auto* env=C3D_GetTexEnv(0);C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,GPU_TEXTURE0,GPU_TEXTURE0);C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);C3D_TexBind(0,&tex);
   C3D_DrawArrays(GPU_TRIANGLES,0,6);GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH,1);C3D_FrameEnd(0);C3Di_RenderQueueWaitDone();
   const auto* out=static_cast<const uint8_t*>(target->frameBuf.colorBuf);unsigned bad=0;
   if(fmt==0&&levelCase<0){if(FILE* raw=fopen(N3DS_ROOT "/texture-selftest.rgba","wb")){std::vector<uint8_t> readback(64*64*4);memcpy(readback.data(),out,readback.size());fwrite(readback.data(),1,readback.size(),raw);fclose(raw);}}
   for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){uint8_t expected[4];t2p::picaSampleTexel(fmt,encoded[levelCase<0?0:levelCase].data(),levelCase<0?8:base>>levelCase,levelCase<0?x:0,levelCase<0?7-y:0,expected);auto* actual=out+tiled(x*8+4,y*8+4)*4;for(unsigned c=0;c<4;++c)if(actual[3-c]!=expected[c]){if(!bad){char m[160];snprintf(m,sizeof m,"FIRST %s xy%u,%u expected %u,%u,%u,%u actual %u,%u,%u,%u\n",t2p::picaTexFmtName(fmt),x,y,expected[0],expected[1],expected[2],expected[3],actual[3],actual[2],actual[1],actual[0]);line(m);}++bad;break;}}
   char msg[192];snprintf(msg,sizeof msg,"FORMAT %s forced-level %d: %u /64 texels differ\n",t2p::picaTexFmtName(fmt),levelCase,bad);line(msg);errors+=bad;C3D_TexDelete(&tex);
  }
 }
 // Exercise the real screen-buffer-as-texture path, with a power-of-two control.
 // Both paths draw the same 8x8 nearest pattern and sample it back at 64x64.
 if(target&&shaderOK&&verts){
  std::vector<uint8_t> input(8*8*4),encoded;
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){auto* p=input.data()+(y*8+x)*4;p[0]=x*31;p[1]=y*31;p[2]=(x^y)*31;p[3]=255;}
  t2p::picaEncode(0,input,8,8,encoded,true,false);
  C3D_Tex pattern{};bool patternOK=C3D_TexInit(&pattern,8,8,GPU_RGBA8);
  if(!patternOK){line("ERROR screen-copy pattern allocation\n");++errors;}
  else{
   C3D_TexLoadImage(&pattern,encoded.data(),GPU_TEXFACE_2D,0);C3D_TexFlush(&pattern);C3D_TexSetFilter(&pattern,GPU_NEAREST,GPU_NEAREST);C3D_TexSetWrap(&pattern,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
   auto drawPattern=[&](C3D_RenderTarget* dst,C3D_Tex* tex){
    C3D_FrameDrawOn(dst);C3D_BindProgram(&program);
    auto* ai=C3D_GetAttrInfo();AttrInfo_Init(ai);AttrInfo_AddLoader(ai,0,GPU_FLOAT,4);AttrInfo_AddLoader(ai,1,GPU_FLOAT,4);
    auto* bi=C3D_GetBufInfo();BufInfo_Init(bi);BufInfo_Add(bi,verts,sizeof(Vertex),2,0x10);
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);C3D_CullFace(GPU_CULL_NONE);C3D_DepthMap(true,1,1);C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);C3D_AlphaTest(false,GPU_ALWAYS,0);C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
    for(int j=0;j<6;++j)C3D_TexEnvInit(C3D_GetTexEnv(j));auto* e=C3D_GetTexEnv(0);C3D_TexEnvSrc(e,C3D_Both,GPU_TEXTURE0,GPU_TEXTURE0,GPU_TEXTURE0);C3D_TexEnvFunc(e,C3D_Both,GPU_REPLACE);C3D_TexBind(0,tex);C3D_DrawArrays(GPU_TRIANGLES,0,6);
   };
   std::vector<uint8_t> reference[2][3];
   for(int dims=0;dims<2;++dims){
    const int sw=dims?240:256,sh=dims?400:512;
    auto* srcTarget=C3D_RenderTargetCreate(sw,sh,GPU_RB_RGBA8,-1);
    if(!srcTarget){line("ERROR screen-copy source allocation\n");++errors;continue;}
    C3D_Tex view{};view.data=srcTarget->frameBuf.colorBuf;view.width=sw;view.height=sh;view.size=sw*sh*4;view.fmt=GPU_RGBA8;view.param=GPU_TEXTURE_MODE(GPU_TEX_2D);C3D_TexSetFilter(&view,GPU_NEAREST,GPU_NEAREST);C3D_TexSetWrap(&view,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    for(int fenced=0;fenced<2;++fenced)for(int repeat=0;repeat<3;++repeat){
     for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){auto* p=input.data()+(y*8+x)*4;p[0]=((x+repeat*3)&7)*31;p[1]=((y+repeat*2)&7)*31;p[2]=(x^y^repeat)*31;p[3]=255;}
     t2p::picaEncode(0,input,8,8,encoded,true,false);C3D_TexLoadImage(&pattern,encoded.data(),GPU_TEXFACE_2D,0);C3D_TexFlush(&pattern);
     C3D_FrameBegin(C3D_FRAME_SYNCDRAW);drawPattern(srcTarget,&pattern);
     if(fenced){C3D_FrameEnd(0);C3Di_RenderQueueWaitDone();C3D_FrameBegin(C3D_FRAME_SYNCDRAW);}
     drawPattern(target,&view);GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH,1);C3D_FrameEnd(0);C3Di_RenderQueueWaitDone();
     std::vector<uint8_t> pixels(64*64*4);memcpy(pixels.data(),target->frameBuf.colorBuf,pixels.size());
     unsigned bad=0;if(!dims)reference[fenced][repeat]=pixels;
     const auto& expected=reference[fenced][repeat];
     for(size_t i=0;i<pixels.size();i+=4)if(memcmp(pixels.data()+i,expected.data()+i,4))++bad;
     // Independent centre-of-cell oracle also catches a uniformly wrong control.
     unsigned oracleBad=0;for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){auto* actual=pixels.data()+tiled(x*8+4,y*8+4)*4;auto* wanted=input.data()+(y*8+x)*4;for(unsigned c=0;c<4;++c)if(actual[3-c]!=wanted[c]){++oracleBad;break;}}
     char msg[192];snprintf(msg,sizeof msg,"SCREEN_COPY %dx%d fenced %d repeat %d: %u /4096 pixels differ from control; oracle %u /64\n",sw,sh,fenced,repeat,bad,oracleBad);line(msg);errors+=bad+oracleBad;
    }
    // Also exercise the production EFB planner, backend, cache and padded target.
    {
     t2p::Backend3ds backend;t2p::TextureCache cache;t2p::C3dShaderCache shaders;t2p::TexEntry mainView;
     if(!backend.init(512u<<10)||!backend.setMainView(mainView,srcTarget)){line("ERROR production-copy backend setup\n");++errors;}
     else{
      cache.setEvictCallback([&](const t2p::TexEntry& e){backend.release(e);});
      t2p::FrameConfig cfg;cfg.targetW=sh;cfg.targetH=sw;cfg.efbW=640;cfg.efbH=480;cfg.fit=t2p::FitMode::Stretch;cfg.rotate=true;cfg.resolve();
      t2p::GxCopy c;c.srcW=640;c.srcH=480;c.fmt=6;c.dstAddr=0x1000;t2p::GxState state;state.pixelFmt=1;
      auto plan=t2p::planEfbCopy(c,state,cfg,shaders,&mainView);
      C3D_FrameBegin(C3D_FRAME_SYNCDRAW);backend.beginFrame();drawPattern(srcTarget,&pattern);
      const bool copied=backend.copy(plan,cache,srcTarget);C3D_FrameEnd(0);C3Di_RenderQueueWaitDone();
      // Framebuffer row zero corresponds to texture v=1 (frame.h); the
      // unrotated copy target stores logical y directly, including bottom padding.
      unsigned bad=0;const void* raw=nullptr;uint32_t bytes=0;const auto* entry=cache.renderedAt(c.dstAddr);
      if(!copied||!entry||!backend.textureImage(*entry,&raw,&bytes)){line("ERROR production-copy execution\n");++errors;}
      else{
       for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){unsigned xp=(2*x+1)*plan.tex.portW/16,yp=(2*y+1)*plan.tex.portH/16;uint8_t expected[4],actual[4];
        t2p::picaSampleTexel(0,static_cast<const uint8_t*>(srcTarget->frameBuf.colorBuf),sw,sw-1-yp,xp,expected);
        t2p::picaSampleTexel(0,static_cast<const uint8_t*>(raw),plan.tex.allocW,xp,yp,actual);
        if(memcmp(expected,actual,4)){if(!bad){char m[200];snprintf(m,sizeof m,"FIRST production-copy xy %u,%u expected %u,%u,%u,%u actual %u,%u,%u,%u\n",x,y,expected[0],expected[1],expected[2],expected[3],actual[0],actual[1],actual[2],actual[3]);line(m);}++bad;}
       }
       char m[180];snprintf(m,sizeof m,"PRODUCTION_COPY %dx%d padded %dx%d: %u /64 texels differ\n",sw,sh,plan.tex.allocW,plan.tex.allocH,bad);line(m);errors+=bad;
      }
      cache.invalidateRange(c.dstAddr,1);backend.release(mainView);
     }
     shaders.forEach([&](t2p::C3dShader& sh){backend.releaseShader(sh);});backend.shutdown();
    }
    C3D_RenderTargetDelete(srcTarget);
   }
   C3D_TexDelete(&pattern);
  }
 }
 C3Di_RenderQueueWaitDone();if(verts)linearFree(verts);shaderProgramFree(&program);if(dv)DVLB_Free(dv);if(target)C3D_RenderTargetDelete(target);C3D_Fini();
 char done[80];snprintf(done,sizeof done,"DONE errors %u\n",errors);line(done);if(report)fclose(report);if(replay)fclose(replay);
 consoleInit(GFX_BOTTOM,nullptr);printf("Windfall GPU check\n\n%u mismatched texels\n\nReport saved:\n3ds/ww3ds/texture-selftest.txt\n\nSTART: close",errors);
 while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START)break;gfxFlushBuffers();gfxSwapBuffers();gspWaitForVBlank();}
 return errors?1:0;
}
}
