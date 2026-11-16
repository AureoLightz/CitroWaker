#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <3ds.h>
#include "bottom_ui_texture_3ds.h"
#include "ui_fade_3ds.h"
#include "ui_shortcuts_ctru_3ds.h"
#include "motion_ctru_3ds.h"
#include "settings_overlay_3ds.h"

// Precomposed fallback panel, embedded in framebuffer order. Repaint only on
// lifecycle changes or the optional 4 Hz FPS counter; no additional GPU draws.
class BottomUi3ds {
  bool dirty_=false,painted_=false;
  uint8_t paintedAlpha_=0;
  uint64_t fpsTick_=0;char fpsText_[24]={};bool paintedFps_=false;
  void paint(uint8_t alpha){
    if(n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire))return;
    u16 w=0,h=0;
    auto* p=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h);
    if(!p || w!=240 || h!=320 || gfxGetScreenFormat(GFX_BOTTOM)!=GSP_RGB565_OES)return;
    if(alpha==255)std::memcpy(p,kBottomUiTexture,sizeof kBottomUiTexture);
    else if(!alpha)std::memset(p,0,sizeof kBottomUiTexture);
    else n3ds_ui::fade565(reinterpret_cast<uint16_t*>(p),
      reinterpret_cast<const uint16_t*>(kBottomUiTexture),sizeof kBottomUiTexture/2,alpha);
    if(alpha){
      auto colour=[alpha](unsigned rgb){unsigned r=((rgb>>16)&255)*alpha/255,g=((rgb>>8)&255)*alpha/255,b=(rgb&255)*alpha/255;return (r<<16)|(g<<8)|b;};
      citro_settings::Canvas c{reinterpret_cast<uint16_t*>(p)};c.rect(20,206,280,26,colour(0x173d4a));c.frame(20,206,280,26,colour(0x547c85));c.text(31,212,"START + SELECT",colour(0x9dc1c6));c.text(235,212,"Settings",colour(0xffe3a1));
    }
    if(alpha==255&&citro_settings::fpsEnabled.load(std::memory_order_relaxed)){
      citro_settings::Canvas c{reinterpret_cast<uint16_t*>(p)};c.rect(226,9,83,20,0x102f3e);c.frame(226,9,83,20,0x609a9d);c.center(226,12,83,fpsText_,0xffe6a3);
    }
    paintedFps_=citro_settings::fpsEnabled.load(std::memory_order_relaxed);
    paintedAlpha_=alpha;
    dirty_=true;painted_=true;
  }
public:
  void init(){paint(0);}
  void update(unsigned long,double ms){
    bool fpsChanged=false;const bool fps=citro_settings::fpsEnabled.load(std::memory_order_relaxed);const uint64_t now=fps?svcGetSystemTick():0;
    if(fps&&(!fpsTick_||now-fpsTick_>SYSCLOCK_ARM11/4)){char next[24];std::snprintf(next,sizeof next,"%.1f FPS",ms>0?1000.0/ms:0.0);fpsChanged=std::strcmp(next,fpsText_)!=0;std::strcpy(fpsText_,next);fpsTick_=now;}

    if(n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire)){painted_=false;dirty_=false;}
    else {
      const uint8_t alpha=n3ds_ui::Presentation{n3ds_ui::presentationWord.load(std::memory_order_acquire)}.alpha();
      if(!painted_||alpha!=paintedAlpha_||fpsChanged||fps!=paintedFps_)paint(alpha);
    }
  }
  bool takeDirty(){
    if(n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire)){painted_=false;dirty_=false;return false;}
    bool d=dirty_;dirty_=false;return d;
  }
  void capture(unsigned long frame){
    static bool requested=std::getenv("N3DS_UI_CAPTURE")!=nullptr;
    if(!requested || frame!=620 || n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire))return;
    u16 w=0,h=0;const u8* p=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h);
    if(gfxGetScreenFormat(GFX_BOTTOM)!=GSP_RGB565_OES)return;
    if(FILE* f=std::fopen("sdmc:/3ds/ww3ds/ui.rgb565","wb")){
      std::fwrite(p,1,size_t(w)*h*2,f);std::fclose(f);
    }
  }
};
