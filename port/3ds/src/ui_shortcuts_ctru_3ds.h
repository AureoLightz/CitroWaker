// Included by bottom_ui_3ds.h in its single libctru translation unit.
// Exported input function is called by PADRead after the existing hidScanInput.
#pragma once
#include <3ds.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include "ui_shortcuts_3ds.h"
#include "ui_shortcut_config_3ds.h"
#include "ui_lifecycle_3ds.h"
#include "ui_fade_3ds.h"
namespace n3ds_ui {
inline std::atomic<uint32_t> enabled{0},highlight{0},items{0x00ffffff},bindings{0x00ffffff},invalidated{0};
inline std::atomic<bool> gpuOwnsBottom{false};
inline std::atomic<bool> panelOn{false};   // (Claude, release prep) bottom panel shown (performance.ini bottom_ui): its column buttons take touches
inline std::atomic<uint32_t> presentationWord{0},presentationMask{allActions};
inline Touch gesture;
inline ShortcutConfig shortcuts;
inline std::atomic<int> pressedSlot{-1};
}
extern "C" void n3dsUiSetTouchEnabled(uint32_t actions){
 const uint32_t usable=n3ds_ui::gpuOwnsBottom.load(std::memory_order_acquire)?actions&n3ds_ui::allActions:0;
 const uint32_t old=n3ds_ui::enabled.load(std::memory_order_relaxed);
 if(old==usable)return;
 n3ds_ui::enabled.store(usable,std::memory_order_release);
 if(old&~usable)n3ds_ui::invalidated.fetch_or(old&~usable,std::memory_order_release);
 n3ds_ui::highlight.fetch_and(usable,std::memory_order_relaxed);
 if(!n3ds_ui::highlight.load(std::memory_order_relaxed))n3ds_ui::pressedSlot.store(-1,std::memory_order_relaxed);
}
extern "C" void n3dsUiSetGpuOwnsBottom(int owns){
 n3ds_ui::gpuOwnsBottom.store(owns!=0,std::memory_order_release);
 if(!owns)n3dsUiSetTouchEnabled(0);
}
extern "C" uint32_t n3dsUiTouchAction(uint32_t held,int blocked){
 // (Claude, release prep) the column buttons are drawn whenever the panel is on (CPU panel or live map): what the
 // game's HUD shows as usable (d_meter.cpp dMeter_publishBottomUi) can be touched. Before, only n3dsUiSetTouchEnabled
 // (never called) enabled them, so touches did nothing.
 const uint32_t enabled=n3ds_ui::panelOn.load(std::memory_order_acquire)?
  n3ds_ui::actionsForItems(n3ds_ui::presentationMask.load(std::memory_order_acquire),n3ds_ui::items.load(std::memory_order_acquire)):0;
 if(n3ds_ui::invalidated.load(std::memory_order_acquire))
  n3ds_ui::gesture.invalidate(n3ds_ui::invalidated.exchange(0,std::memory_order_acq_rel));
 bool down=(held&KEY_TOUCH)!=0;touchPosition pos{};
 if(enabled&&!blocked&&down)hidTouchRead(&pos);
 {  // TEST (env N3DS_TOUCH_TEST="from to x y"): a touch held for PADRead calls [from, to)
  static unsigned calls=0;static int t[4]={-1,-1,0,0};static bool parsed=false;
  if(!parsed){parsed=true;if(const char* v=std::getenv("N3DS_TOUCH_TEST"))std::sscanf(v,"%d %d %d %d",&t[0],&t[1],&t[2],&t[3]);}
  const int c=int(calls++);if(t[0]>=0&&c>=t[0]&&c<t[1]){down=true;pos.px=u16(t[2]);pos.py=u16(t[3]);}
 }
 const uint32_t action=n3ds_ui::gesture.update(down,int(pos.px),int(pos.py),enabled,blocked!=0,n3ds_ui::shortcuts.slots);
 n3ds_ui::highlight.store(action,std::memory_order_relaxed);
 n3ds_ui::pressedSlot.store(n3ds_ui::gesture.pressedSlot(),std::memory_order_relaxed);return action;
}
extern "C" uint32_t n3dsUiTouchHighlight(void){return n3ds_ui::highlight.load(std::memory_order_relaxed);}

extern "C" void n3dsUiPublishItems(uint8_t x,uint8_t y,uint8_t z){
 const uint32_t packed=uint32_t(x)|(uint32_t(y)<<8)|(uint32_t(z)<<16);
 const uint32_t old=n3ds_ui::items.exchange(packed,std::memory_order_acq_rel);
 const uint32_t changed=n3ds_ui::changedItems(old,packed);
 if(changed)n3ds_ui::invalidated.fetch_or(changed,std::memory_order_release);
}
extern "C" uint32_t n3dsUiGetItems(void){return n3ds_ui::items.load(std::memory_order_acquire);}

extern "C" void n3dsUiPublishBindings(uint8_t x,uint8_t y,uint8_t z){
 const uint32_t packed=uint32_t(x)|(uint32_t(y)<<8)|(uint32_t(z)<<16);
 const uint32_t old=n3ds_ui::bindings.exchange(packed,std::memory_order_acq_rel);
 uint32_t changed=0;for(unsigned i=0;i<3;++i)if(((old^packed)>>(8*i))&255u)changed|=1u<<i;
 if(changed)n3ds_ui::invalidated.fetch_or(changed,std::memory_order_release);
}

// Called once by PADInit. No per-frame file access and no live reload while held.
extern "C" int n3dsUiLoadShortcuts(void){
 static bool loaded=false;static int result=-1;
 if(!loaded){loaded=true;result=n3ds_ui::shortcuts.load("sdmc:/3ds/ww3ds/shortcuts.ini");
  if(result==0){n3ds_ui::invalidated.fetch_or(n3ds_ui::allActions,std::memory_order_release);
   n3ds_ui::highlight.store(0,std::memory_order_relaxed);n3ds_ui::pressedSlot.store(-1,std::memory_order_relaxed);}
 }
 return result;
}
extern "C" int n3dsUiGetShortcut(unsigned slot,int rect[4],uint32_t* action){
 if(slot>=5||!rect||!action)return 0;
 const auto& s=n3ds_ui::shortcuts.slots[slot];
 rect[0]=s.rect.x;rect[1]=s.rect.y;rect[2]=s.rect.w;rect[3]=s.rect.h;*action=s.action;return 1;
}
extern "C" int n3dsUiGetTouchSlot(void){return n3ds_ui::pressedSlot.load(std::memory_order_relaxed);}

extern "C" void n3dsUiPublishPresentation(uint8_t alpha,uint32_t drawn,uint32_t usable,uint32_t blocked){
 const auto next=n3ds_ui::presentation(alpha,drawn,usable,blocked);
 n3ds_ui::presentationWord.store(next.word,std::memory_order_release);
 const auto old=n3ds_ui::presentationMask.exchange(next.usable(),std::memory_order_acq_rel);
 const auto removed=old&~next.usable();
 if(removed){
  n3ds_ui::invalidated.fetch_or(removed,std::memory_order_release);
  n3ds_ui::highlight.fetch_and(next.usable(),std::memory_order_relaxed);
  if(!n3ds_ui::highlight.load(std::memory_order_relaxed))n3ds_ui::pressedSlot.store(-1,std::memory_order_relaxed);
 }
}
extern "C" uint32_t n3dsUiGetPresentation(void){return n3ds_ui::presentationWord.load(std::memory_order_acquire);}
