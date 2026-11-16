// Original-GCN panel; pure frame snapshot -> bounded presentation commands.
#pragma once
#include "ui_gcn_assets_3ds.h"
#include "ui_lifecycle_3ds.h"
namespace n3ds_ui {
struct GcnSprite {Rect atlas{},bounds{};bool nineSlice=false,tile=false;uint8_t alpha=255;};
struct GcnItem {Rect bounds{},hintBounds{};Rect hintAtlas{};uint8_t id=255,alpha=0;bool visible=false,enabled=false;};
struct GcnPanel {
 GcnSprite sprites[16]{};unsigned count=0;GcnItem items[3]{};
 uint32_t usable=0;uint8_t alpha=0;
 // map supplied by ordered route events; do not retain last frame's map.
};
inline GcnPanel planGcnPanel(Presentation state,uint32_t packedItems,
 const Target* layout=targets,int touchedSlot=-1,uint32_t stockHintActions=0){
 GcnPanel p{};p.alpha=state.alpha();if(!p.alpha)return p;
 auto add=[&](Rect atlas,Rect bounds,bool nine=false,bool tile=false,uint8_t alpha=255){
  p.sprites[p.count++]={atlas,bounds,nine,tile,alpha};
 };
 add(gcn_art::cloth,{0,0,320,240},false,true);
 add(gcn_art::rail,{224,3,92,196},true);
 for(unsigned i=0;i<3;++i){
  const uint32_t action=layout[i].action;
  const int slot=action==N3DS_UI_ITEM_X?0:action==N3DS_UI_ITEM_Y?1:action==N3DS_UI_ITEM_Z?2:-1;
  auto& item=p.items[i];const int cy=39+64*int(i);
  if(i)add(gcn_art::separator,{234,cy-31,73,2},false,true);
  if(slot<0||!(state.drawn()&action))continue;
  item.id=uint8_t(packedItems>>(8*slot));if(item.id==255)continue;
  item.visible=true;item.enabled=(state.usable()&action)!=0;
  item.alpha=item.enabled?255:102;p.usable|=item.enabled?action:0;
  const bool pressed=item.enabled&&touchedSlot==int(i);
  if(pressed)add(gcn_art::row_pressed,{229,cy-25,82,53});
  item.bounds={263,cy-20+int(pressed),40,40};
  // Hints are stock examples ONLY; actual receiver must replace/omit after
  // buttons.ini remapping. No frame-owned icon resource pointer is stored.
  if(!(stockHintActions&action))continue;
  item.hintAtlas=slot==0?gcn_art::hint_x:slot==1?gcn_art::hint_y:gcn_art::hint_zr;
  item.hintBounds={232,cy-item.hintAtlas.h/2,item.hintAtlas.w,item.hintAtlas.h};
 }
 for(unsigned i=3;i<5;++i){
  const uint32_t action=targets[i].action;if(!(state.drawn()&action))continue;
  const auto b=targets[i].rect;const bool enabled=(state.usable()&action)!=0;
  const bool pressed=enabled&&touchedSlot==int(i);if(enabled)p.usable|=action;
  add(pressed?gcn_art::footer_pressed:gcn_art::footer_normal,b,false,false,enabled?255:102);
  const auto label=i==3?gcn_art::menu:gcn_art::map_zoom;
  add(label,{b.x+(b.w-label.w)/2,b.y+(b.h-label.h)/2+int(pressed),label.w,label.h},false,false,enabled?255:102);
 }
 return p;
}
}
