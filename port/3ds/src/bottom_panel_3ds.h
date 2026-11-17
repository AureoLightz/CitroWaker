// (Claude, release prep 2026-11-17) Bottom-screen touch panel: pure composition, no libctru.
//
// Layout (320x240 landscape, bottom screen):
//   x   0..215  left area: CPU panel (title, hints, Settings button) or, while the live map
//               owns the bottom screen, the GPU's map (only these framebuffer rows are
//               transferred: C3Dx_SetOutputRows(target, kGpuRows)).
//   x 216..319  button column, always painted by the CPU: X / Y / Z items, Menu, Map, FPS.
// The touch targets (n3ds_ui::targets) are the column buttons; ui_shortcuts_3ds.h keeps them.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "settings_canvas_3ds.h"
#include "ui_shortcuts_3ds.h"
#include "ui_lifecycle_3ds.h"
#include "ui_fade_3ds.h"

namespace n3ds_panel {
inline constexpr int kGpuRows = 216;                 // framebuffer rows (= screen columns x) the live map transfer writes
inline constexpr n3ds_ui::Rect kSettingsButton{12,198,196,28};

// Short English names of the items the X / Y / Z buttons can hold (WW item numbers).
inline const char* itemName(uint8_t id){
  switch(id){
  case 0x20:return "Telescope";        case 0x21:return "Tingle Tuner";   case 0x22:return "Wind Waker";
  case 0x23:return "Picto Box";        case 0x24:return "Spoils Bag";     case 0x25:return "Grappling Hook";
  case 0x26:return "Deluxe Picto Box"; case 0x27:return "Bow";            case 0x28:return "Power Bracelets";
  case 0x29:return "Iron Boots";       case 0x2A:return "Magic Armor";    case 0x2C:return "Bait Bag";
  case 0x2D:return "Boomerang";        case 0x2F:return "Hookshot";       case 0x30:return "Delivery Bag";
  case 0x31:return "Bombs";            case 0x33:return "Skull Hammer";   case 0x34:return "Deku Leaf";
  case 0x35:return "Fire & Ice Arrows";case 0x36:return "Light Arrows";
  case 0x50:return "Empty Bottle";     case 0x51:return "Red Potion";     case 0x52:return "Green Potion";
  case 0x53:return "Blue Potion";      case 0x54:return "Half Soup";      case 0x55:return "Elixir Soup";
  case 0x56:return "Water";            case 0x57:return "Fairy";          case 0x58:return "Forest Firefly";
  case 0x59:return "Forest Water";
  case 0x82:case 0x84:case 0x85:case 0x86:case 0x87:case 0x88:return "All-Purpose Bait";
  case 0x83:return "Hyoi Pear";
  case 0x45:return "Skull Necklace";   case 0x46:return "Boko Baba Seed"; case 0x47:return "Golden Feather";
  case 0x48:return "Knight's Crest";   case 0x49:return "Red Jelly";      case 0x4A:return "Green Jelly";
  case 0x4B:return "Blue Jelly";
  case 0x8C:return "Town Flower";      case 0x8D:return "Sea Flower";     case 0x8E:return "Exotic Flower";
  case 0x8F:return "Hero's Flag";      case 0x90:return "Big Catch Flag"; case 0x91:return "Big Sale Flag";
  case 0x92:return "Pinwheel";         case 0x93:return "Sickle Moon Flag";case 0x94:return "Skull Tower Idol";
  case 0x95:return "Fountain Idol";    case 0x96:return "Postman Statue"; case 0x97:return "Shop Guru Statue";
  case 0x98:return "Father's Letter";  case 0x99:return "Note to Mom";    case 0x9A:return "Maggie's Letter";
  case 0x9B:return "Moblin's Letter";  case 0x9C:return "Cabana Deed";    case 0x9D:return "Complimentary ID";
  case 0x9E:return "Fill-Up Coupon";
  default:return "Item";
  }
}

// Everything one picture depends on; equal states give equal pixels.
struct State {
  uint32_t presentation=0;   // n3ds_ui::Presentation word (alpha / drawn / usable / blocked)
  uint32_t items=0x00ffffff; // packed X / Y / Z item numbers, 0xFF = empty
  int pressedSlot=-1;        // touched column slot (0..4) or -1
  bool fps=false; char fpsText[16]={};
  bool mapOwned=false;       // the live map owns x < kGpuRows
  bool settingsPressed=false;
  bool operator==(const State& o)const{return presentation==o.presentation&&items==o.items&&pressedSlot==o.pressedSlot&&fps==o.fps&&
    std::strcmp(fpsText,o.fpsText)==0&&mapOwned==o.mapOwned&&settingsPressed==o.settingsPressed;}
  bool operator!=(const State& o)const{return !(*this==o);}
};

inline unsigned mix(unsigned a,unsigned b,unsigned t){   // t/255 of b over a
  auto ch=[&](int s){const unsigned x=(a>>s)&255,y=(b>>s)&255;return ((x*(255-t)+y*t+127)/255)<<s;};
  return ch(16)|ch(8)|ch(0);
}

// Greedy word wrap into at most two lines of `width` pixels; returns the line count.
inline int wrap2(const citro_settings::Canvas& c,const char* s,int width,char out[2][40]){
  out[0][0]=out[1][0]=0;int line=0;
  for(const char* p=s;*p&&line<2;){
    while(*p==' ')++p;const char* e=p;while(*e&&*e!=' ')++e;
    char next[40];std::snprintf(next,sizeof next,"%s%s%.*s",out[line],out[line][0]?" ":"",int(e-p),p);
    if(c.width(next)<=width||!out[line][0]){std::snprintf(out[line],40,"%s",next);p=e;}
    else ++line;
  }
  return out[1][0]?2:out[0][0]?1:0;
}

inline void button(citro_settings::Canvas& c,n3ds_ui::Rect r,bool enabled,bool pressed){
  const unsigned fill=pressed?0x2f8790:enabled?0x1d4f5c:0x142f39, edge=pressed?0xffe7a0:enabled?0xc9b673:0x3a5a63;
  c.rect(r.x+1,r.y+1,r.w-2,r.h-2,fill);
  c.rect(r.x+2,r.y,r.w-4,1,edge);c.rect(r.x+2,r.y+r.h-1,r.w-4,1,edge);c.rect(r.x,r.y+2,1,r.h-4,edge);c.rect(r.x+r.w-1,r.y+2,1,r.h-4,edge);
  c.put(r.x+1,r.y+1,edge);c.put(r.x+r.w-2,r.y+1,edge);c.put(r.x+1,r.y+r.h-2,edge);c.put(r.x+r.w-2,r.y+r.h-2,edge);
  if(!pressed)c.rect(r.x+2,r.y+r.h-3,r.w-4,1,mix(fill,0x000000,70));
}

inline void badge(citro_settings::Canvas& c,int x,int y,const char* key,bool enabled){
  c.disk(x,y,9,enabled?0xe0cf8c:0x52686c);c.disk(x,y,8,enabled?0x27606b:0x1b3640);
  c.center(x-9,y-9,18,key,enabled?0xfff4cf:0x8aa0a3);
}

// The button column (x >= kGpuRows). Pixels of the left area are left alone.
inline void composeColumn(citro_settings::Canvas& c,const State& s){
  using namespace n3ds_ui;
  const Presentation pr{s.presentation};
  const bool visible=pr.alpha()!=0;
  c.rect(kGpuRows,0,320-kGpuRows,240,0x0e2530);c.rect(kGpuRows,0,1,240,0x30535d);
  if(!visible){if(s.fps)c.center(kGpuRows,221,320-kGpuRows,s.fpsText,0xffe6a3);return;}
  static const char* keys[3]={"X","Y","Z"};
  for(int i=0;i<5;++i){
    const Rect r=targets[i].rect;const uint32_t action=targets[i].action;
    const bool drawn=(pr.drawn()&action)!=0;
    uint8_t id=255;if(i<3)id=uint8_t(s.items>>(8*i));
    const bool has=i>=3||id!=255;
    const bool enabled=drawn&&has&&(pr.usable()&action)!=0;
    const bool pressed=enabled&&s.pressedSlot==i;
    button(c,r,enabled,pressed);
    const int dy=pressed?1:0;
    if(i<3){
      badge(c,r.x+11,r.y+r.h/2+dy,keys[i],enabled);
      const int tx=r.x+21,tw=r.w-23;
      if(!has){c.center(tx,r.y+r.h/2-9+dy,tw,"-",0x52686c);continue;}
      char lines[2][40];const int n=wrap2(c,itemName(id),tw,lines);
      const unsigned col=enabled?0xfdf3d2:0x7d9397;
      if(n==1)c.center(tx,r.y+r.h/2-10+dy,tw,lines[0],col);
      else {c.center(tx,r.y+r.h/2-19+dy,tw,lines[0],col);c.center(tx,r.y+r.h/2-2+dy,tw,lines[1],col);}
    }else{
      c.center(r.x,r.y+5+dy,r.w,i==3?"Menu":"Map",enabled?0xffe6a4:0x7d9397);
    }
  }
  if(s.fps)c.center(kGpuRows,221,320-kGpuRows,s.fpsText,0xffe6a3);
}

// The left area for the CPU panel (no live map).
inline void composeLeft(citro_settings::Canvas& c,const uint16_t* cloth,const State& s){
  for(int x=0;x<kGpuRows;++x)std::memcpy(c.pixels+x*240,cloth+x*240,240*2);
  c.rect(12,10,196,40,0x0d2936);c.frame(12,10,196,40,0x547c85);
  c.center(12,13,196,"CitroWaker",0xffe3a1);
  c.center(12,30,196,"New 3DS",0x9dc1c6);
  c.rect(12,62,196,124,0x0d2936);c.frame(12,62,196,124,0x30535d);
  const char* lines[]={"Touch X, Y or Z to","use the item shown.","Menu opens the pause","menu. For the live map:","Settings > Bottom screen."};
  for(int i=0;i<5;++i)c.text(20,68+i*22,lines[i],0xb9d2d4,184);
  const bool enabled=true;   // the settings menu pauses the game itself: usable in any game state
  button(c,kSettingsButton,enabled,s.settingsPressed);
  c.center(kSettingsButton.x,kSettingsButton.y+5+(s.settingsPressed?1:0),kSettingsButton.w,"Settings  (START+SELECT)",enabled?0xffe6a4:0x7d9397);
}

// Whole picture into `out` (framebuffer order, 240x320). mapOwned: only the column is meaningful.
inline void compose(uint16_t* out,const uint16_t* cloth,const State& s){
  citro_settings::Canvas c{out};
  if(!s.mapOwned){
    // (Claude v137) the left area depends only on the Settings button's pressed state: composed once per state (glyph
    // drawing was the render thread's largest single cost when the FPS text / HUD fade changed the state), then copied
    static uint16_t cache[kGpuRows*240];static int key=-1;
    const int k=s.settingsPressed?1:0;
    if(key!=k){citro_settings::Canvas cc{cache};composeLeft(cc,cloth,s);key=k;}
    std::memcpy(out,cache,sizeof cache);
  }
  composeColumn(c,s);
  const uint8_t alpha=n3ds_ui::Presentation{s.presentation}.alpha();
  // HUD fading in/out (cutscenes, pause, scene changes): the picture fades over black with it. The Settings
  // button stays readable when the game HUD is simply absent (title screen, file select).
  if(alpha!=255&&alpha!=0)n3ds_ui::fade565(out,out,320*240,uint8_t(64+alpha*191/255));
}
}
