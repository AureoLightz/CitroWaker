// UI-owned zero-allocation presentation plan from one immutable frame snapshot.
#pragma once
#include "ui_shortcuts_3ds.h"
#include "ui_panel_assets_3ds.h"
#include "ui_lifecycle_3ds.h"
namespace n3ds_ui {
struct PanelLabel {Rect atlas;Rect bounds;};
struct PanelControl {
 Rect bounds,atlas,frame,icon,hint;
 PanelLabel labels[2]{};
 uint32_t action=0,textColour=0xffffff; // RGB multiplier; atlas retains alpha.
 uint8_t item=255,labelCount=0,iconAlpha=255;
 bool visible=false,enabled=false,pressed=false,nineSlice=true;
};
struct Panel {PanelControl controls[5];uint32_t usable=0;uint8_t alpha=255;};
// Empty/disabled slots remain stationary. Only enabled actions enter the touch
// mask. Custom profiles must pass the touched SLOT, so duplicate actions do not
// falsely highlight both controls. No live game pointers or renderer types.
inline Panel planPanel(bool hudAvailable,uint32_t drawnActions,uint32_t packedItems,
 uint32_t heldHighlight,uint32_t usableActions=allActions,
 const Target* layout=targets,int touchedSlot=-2){
 Panel p{};uint32_t assigned=0;
 for(unsigned i=0;i<5;++i)assigned|=i<3?layout[i].action:targets[i].action;
 p.usable=hudAvailable?actionsForItems(drawnActions&usableActions&assigned&allActions,packedItems):0;
 for(unsigned i=0;i<5;++i){
  auto& c=p.controls[i];c.bounds=targets[i].rect;c.action=i<3?layout[i].action:targets[i].action;
  const auto& b=c.bounds;
  c.visible=hudAvailable&&(!c.action||(drawnActions&c.action)!=0);
  c.enabled=(p.usable&c.action)!=0;
  c.pressed=c.enabled&&(touchedSlot>=-1?int(i)==touchedSlot:
    layout==targets&&(heldHighlight&c.action)!=0);
  c.iconAlpha=c.enabled?255:153;
  const int itemSlot=c.action==N3DS_UI_ITEM_X?0:c.action==N3DS_UI_ITEM_Y?1:c.action==N3DS_UI_ITEM_Z?2:-1;
  c.item=itemSlot>=0?uint8_t(packedItems>>(8*itemSlot)):uint8_t(255);
  c.frame=b;
  auto label=[&](Rect source,int x,int y){
   auto& l=c.labels[c.labelCount++];l.atlas=source;l.bounds={x,y,source.w,source.h};
  };
  if(itemSlot>=0&&b.h>=48){
   c.atlas=c.pressed?art::WellPressed:art::WellNormal;c.nineSlice=false;
   const int cx=b.x+42,cy=b.y+b.h/2;
   c.frame={cx-25,cy-25,50,50};c.icon={cx-20,cy-20,40,40};
   c.hint={b.x,cy-8,16,16};
  }else if(c.action==N3DS_UI_MENU){
   c.atlas=c.pressed?art::FrameMenuPressed:art::FrameMenuNormal;
   label(art::LabelMenu,b.x+(b.w-art::LabelMenu.w)/2,
     b.y+(b.h-art::LabelMenu.h)/2+int(c.pressed));
  }else if(c.action==N3DS_UI_MAP_SIZE){
   c.atlas=c.pressed?art::FrameMapPressed:art::FrameMapNormal;
   label(art::LabelMapZoom,b.x+(b.w-art::LabelMapZoom.w)/2,
     b.y+(b.h-art::LabelMapZoom.h)/2+int(c.pressed));
  }else{
   c.atlas=art::FrameEmptyNormal;
   if(b.h>=48)c.frame={b.x+17,b.y+2,50,50};
  }
  if(itemSlot>=0&&c.item==255){
   c.icon={0,0,0,0};c.hint={0,0,0,0};c.labelCount=0;
   c.atlas=art::FrameEmptyNormal;c.nineSlice=true;
  }
 }
 return p;
}
// Consumer accepts the frame's recorded lifecycle snapshot. alpha multiplies the
// entire bottom composition (background, route/map and controls) over BLACK.
// Never reconstruct this from mutable globals on the render thread.
inline Panel planPanel(Presentation state,uint32_t packedItems,uint32_t highlight,
 const Target* layout=targets,int touchedSlot=-2){
 auto p=planPanel(state.alpha()!=0,state.drawn(),packedItems,highlight,state.usable(),layout,touchedSlot);
 p.alpha=state.alpha();
 return p;
}

}
