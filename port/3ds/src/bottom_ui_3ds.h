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

#include "bottom_panel_3ds.h"

// (Claude, release prep) Bottom-screen touch panel (bottom_panel_3ds.h). All calls run on the render owner thread.
// CPU mode: the whole picture is composed into a shadow buffer, copied into the back framebuffer and swapped by the
// end-of-frame code (takeDirty). Live map mode: the GPU transfer writes x < kGpuRows; paintColumn (frame-begin hook)
// keeps the button column of BOTH framebuffers current (each buffer is refreshed when it is the back buffer).
class BottomUi3ds {
  alignas(8) static inline uint16_t shadow_[320*240];
  n3ds_panel::State shown_{};bool painted_=false,dirty_=false;
  uint64_t fpsTick_=0;char fpsText_[16]={};
  uint32_t colVersion_=1;const void* colFb_[2]={};uint32_t colFbVersion_[2]={};n3ds_panel::State colState_{};bool colValid_=false;
  n3ds_panel::State current(bool mapOwned)const{
    n3ds_panel::State s;
    s.presentation=n3ds_ui::presentationWord.load(std::memory_order_acquire);
    s.items=n3ds_ui::items.load(std::memory_order_acquire);
    s.pressedSlot=n3ds_ui::pressedSlot.load(std::memory_order_relaxed);
    s.fps=citro_settings::fpsEnabled.load(std::memory_order_relaxed);
    if(s.fps)std::memcpy(s.fpsText,fpsText_,sizeof s.fpsText);
    s.mapOwned=mapOwned;
    return s;
  }
  void paint(const n3ds_panel::State& s){
    u16 w=0,h=0;auto* p=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h);
    if(!p||w!=240||h!=320||gfxGetScreenFormat(GFX_BOTTOM)!=GSP_RGB565_OES)return;
    n3ds_panel::compose(shadow_,kBottomUiTexture,s);
    std::memcpy(p,shadow_,sizeof shadow_);
    shown_=s;painted_=true;dirty_=true;
  }
public:
  // Black bottom screen (settings menu restore, start-up); the next update() repaints.
  void init(){
    u16 w=0,h=0;auto* p=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h);
    if(p&&w==240&&h==320&&!n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire))std::memset(p,0,size_t(w)*h*2);
    painted_=false;dirty_=true;colValid_=false;colFb_[0]=colFb_[1]=nullptr;
  }
  void ownerChanged(){painted_=false;colValid_=false;colFb_[0]=colFb_[1]=nullptr;}
  void update(unsigned long,double ms){
    const bool fps=citro_settings::fpsEnabled.load(std::memory_order_relaxed);
    if(fps){const uint64_t now=svcGetSystemTick();if(!fpsTick_||now-fpsTick_>SYSCLOCK_ARM11/4){std::snprintf(fpsText_,sizeof fpsText_,"%.1f FPS",ms>0?1000.0/ms:0.0);fpsTick_=now;}}
    if(n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire)){painted_=false;dirty_=false;return;}
    const auto s=current(false);
    if(!painted_||s!=shown_)paint(s);
  }
  // Frame-begin hook while the live map owns the bottom screen: the column of the back buffer this frame's
  // transfer goes to. Cost: a 104 x 240 RGB565 copy (50 KB) only when that buffer is out of date.
  void paintColumn(){
    if(!n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire))return;
    u16 w=0,h=0;auto* p=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h);
    if(!p||w!=240||h!=320||gfxGetScreenFormat(GFX_BOTTOM)!=GSP_RGB565_OES)return;
    const auto s=current(true);
    if(!colValid_||s!=colState_){n3ds_panel::State c=s;citro_settings::Canvas cv{shadow_};n3ds_panel::composeColumn(cv,c);
      const uint8_t a=n3ds_ui::Presentation{s.presentation}.alpha();
      if(a!=255&&a!=0)n3ds_ui::fade565(shadow_+n3ds_panel::kGpuRows*240,shadow_+n3ds_panel::kGpuRows*240,(320-n3ds_panel::kGpuRows)*240,uint8_t(64+a*191/255));
      colState_=s;colValid_=true;++colVersion_;}
    const int slot=colFb_[0]==p?0:colFb_[1]==p?1:(colFb_[0]?1:0);
    if(colFb_[slot]==p&&colFbVersion_[slot]==colVersion_)return;
    uint16_t* dst=reinterpret_cast<uint16_t*>(p)+n3ds_panel::kGpuRows*240;
    std::memcpy(dst,shadow_+n3ds_panel::kGpuRows*240,size_t(320-n3ds_panel::kGpuRows)*240*2);
    GSPGPU_FlushDataCache(dst,u32(320-n3ds_panel::kGpuRows)*240*2);
    colFb_[slot]=p;colFbVersion_[slot]=colVersion_;
  }
  // N3DS_UI_CAPTURE with the live map: the bottom framebuffer the previous frame's transfer completed (map + column).
  void captureRouted(unsigned long frame){
    static const unsigned long at=[]{const char* v=std::getenv("N3DS_UI_CAPTURE");return v?std::strtoul(v,nullptr,10):0ul;}();
    static const void* prev=nullptr;
    if(at&&frame==at+1&&prev){GSPGPU_InvalidateDataCache(const_cast<void*>(prev),320*240*2);static uint16_t host[320*240];std::memcpy(host,prev,sizeof host);if(FILE* f=std::fopen("sdmc:/3ds/ww3ds/ui_routed.rgb565","wb")){std::fwrite(host,1,sizeof host,f);std::fclose(f);}}
    prev=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr);
  }
  bool takeDirty(){
    if(n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire)){painted_=false;dirty_=false;return false;}
    bool d=dirty_;dirty_=false;return d;
  }
  // N3DS_UI_CAPTURE=frame (default 620): the composed picture (CPU mode) to ui.rgb565; with the live map the column
  // only (the map half is the GPU's: ui_route_shots.bin).
  void capture(unsigned long frame){
    static const unsigned long at=[]{const char* v=std::getenv("N3DS_UI_CAPTURE");return v?(std::strtoul(v,nullptr,10)?std::strtoul(v,nullptr,10):620ul):0ul;}();
    if(!at||frame!=at)return;
    if(FILE* f=std::fopen("sdmc:/3ds/ww3ds/ui.rgb565","wb")){std::fwrite(shadow_,1,sizeof shadow_,f);std::fclose(f);}
  }
};
