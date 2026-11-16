// Pure layout/gesture state: no libctru, allocations, files, timing or GPU work.
#pragma once
#include <stdint.h>
#include "ui_shortcuts_api_3ds.h"
namespace n3ds_ui {
struct Rect {
 int x,y,w,h;
 constexpr bool contains(int px,int py)const{return px>=x&&py>=y&&px<x+w&&py<y+h;}
};
// The map's existing 8-pixel frame scales outside this content rectangle.
// Default content is120px:176/120 scaling leaves the8px frame within
// approximately x12..212,y0..200. Footer starts at204.
inline constexpr Rect mapContent{24,12,176,176};
struct Target {Rect rect;uint32_t action;};
inline constexpr Target targets[]={
 {{240,12,72,54},N3DS_UI_ITEM_X},{{240,76,72,54},N3DS_UI_ITEM_Y},
 {{240,140,72,54},N3DS_UI_ITEM_Z},{{12,204,144,30},N3DS_UI_MENU},
 {{164,204,144,30},N3DS_UI_MAP_SIZE}
};
inline constexpr uint32_t allActions=N3DS_UI_ITEM_X|N3DS_UI_ITEM_Y|N3DS_UI_ITEM_Z|N3DS_UI_MENU|N3DS_UI_MAP_SIZE;
inline uint32_t actionsForItems(uint32_t enabled,uint32_t items){
 for(unsigned i=0;i<3;++i)if(((items>>(8*i))&255u)==255u)enabled&=~(1u<<i);
 return enabled;
}
inline uint32_t hit(int x,int y,uint32_t enabled,const Target* layout=targets){
 if(x<0||y<0||x>=320||y>=240)return N3DS_UI_NONE;
 for(unsigned i=0;i<5;++i){const auto& t=layout[i];if((t.action&enabled)&&t.rect.contains(x,y))return t.action;}
 return N3DS_UI_NONE;
}
// New touches must hit the drawn target. A captured hold tolerates small hand
// jitter, without broadening initial hits or allowing slide-to-another-button.
inline bool insideCaptured(uint32_t action,int x,int y){
 constexpr int tolerance=4;
 for(const auto& t:targets)if(t.action==action){
  const Rect release{t.rect.x-tolerance,t.rect.y-tolerance,
                     t.rect.w+2*tolerance,t.rect.h+2*tolerance};
  return release.contains(x,y);
 }
 return false;
}
class Touch {
 bool down_=false,cancelled_=false;uint32_t captured_=0;
 int slot_=-1;Rect capturedRect_{};
public:
 void invalidate(uint32_t actions){if(captured_&actions){captured_=0;slot_=-1;cancelled_=true;}}
 int pressedSlot()const{return captured_?slot_:-1;}
 uint32_t update(bool down,int x,int y,uint32_t enabled,bool blocked=false,const Target* layout=targets){
  if(!down){down_=false;cancelled_=false;captured_=0;slot_=-1;return 0;}
  enabled=blocked?0:enabled&allActions;
  if(!down_){
   down_=true;captured_=0;slot_=-1;
   if(x>=0&&y>=0&&x<320&&y<240)for(unsigned i=0;i<5;++i){
    const auto& t=layout[i];if((t.action&enabled)&&t.rect.contains(x,y)){
     captured_=t.action;slot_=int(i);capturedRect_=t.rect;break;
    }
   }
   cancelled_=captured_==0;
  }
  if(cancelled_)return 0;
  const Rect release{capturedRect_.x-4,capturedRect_.y-4,capturedRect_.w+8,capturedRect_.h+8};
  const auto& current=layout[slot_];
  if(!(enabled&captured_)||current.action!=captured_||
     current.rect.x!=capturedRect_.x||current.rect.y!=capturedRect_.y||
     current.rect.w!=capturedRect_.w||current.rect.h!=capturedRect_.h||
     !release.contains(x,y)){
   cancelled_=true;captured_=0;slot_=-1;return 0;
  }
  return captured_;
 }
};
}
