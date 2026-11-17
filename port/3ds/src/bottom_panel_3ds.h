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
#include <algorithm>
#include "settings_canvas_3ds.h"
#include "ui_shortcuts_3ds.h"
#include "ui_lifecycle_3ds.h"
#include "ui_fade_3ds.h"

namespace n3ds_panel {
inline constexpr int kGpuRows = 216;                 // framebuffer rows (= screen columns x) the live map transfer writes
inline constexpr n3ds_ui::Rect kSettingsButton{12,198,192,30};

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

// (Claude v140) layout helpers measured on the font: capitals have ink on cell rows 3..11 of 18, so a line whose capitals are centred
// on cy starts at cy - kCapMid; widths drop the 1-pixel trailing advance so centring follows the ink.
inline constexpr int kCapMid=7;
inline int inkWidth(const citro_settings::Canvas& c,const char* s){const int w=c.width(s);return w>0?w-1:0;}
inline void textMid(citro_settings::Canvas& c,int x,int cy,const char* s,unsigned col,int maxw=320){c.text(x,cy-kCapMid,s,col,maxw);}
inline void centerMid(citro_settings::Canvas& c,int x,int cy,int w,const char* s,unsigned col){c.text(x+(w-inkWidth(c,s))/2,cy-kCapMid,s,col,w);}

inline unsigned readPx(const citro_settings::Canvas& c,int x,int y){
  const uint16_t p=c.pixels[x*240+239-y];
  return ((((p>>11)*255+15)/31)<<16)|(((((p>>5)&63)*255+31)/63)<<8)|(((p&31)*255+15)/31);
}
inline void blendPx(citro_settings::Canvas& c,int x,int y,unsigned col,unsigned a){
  if(x<0||x>=320||y<0||y>=240||!a)return;
  c.put(x,y,a>=255?col:mix(readPx(c,x,y),col,a));
}
// Anti-aliased filled circle (centre on a pixel centre), optional ring of width 1.5 in `edge`.
inline void aaDisk(citro_settings::Canvas& c,float cx,float cy,float r,unsigned fill,unsigned edge){
  const int x0=int(cx-r-2),x1=int(cx+r+2),y0=int(cy-r-2),y1=int(cy+r+2);
  for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
    const float dx=x+0.5f-cx,dy=y+0.5f-cy;float d=dx*dx+dy*dy;
    // sqrt by two Newton steps from a linear guess is plenty for a 1-pixel edge
    float q=d>0?d:0;float sq=q>0?q*0.5f+0.5f:0;if(q>0){sq=0.5f*(sq+q/sq);sq=0.5f*(sq+q/sq);sq=0.5f*(sq+q/sq);sq=0.5f*(sq+q/sq);}
    const float outside=sq-r;   // <0 inside
    const float cover=outside<=-0.5f?1.f:outside>=0.5f?0.f:0.5f-outside;
    if(cover<=0)continue;
    const float ringIn=sq-(r-1.5f);
    const float ringCover=ringIn<=-0.5f?0.f:ringIn>=0.5f?1.f:ringIn+0.5f;
    const unsigned col=mix(fill,edge,unsigned(ringCover*255.f+0.5f));
    blendPx(c,x,y,col,unsigned(cover*255.f+0.5f));
  }
}
// Rounded rectangle: 1-pixel `edge` outline, `fill` inside, corner radius r (anti-aliased corners).
inline void roundRect(citro_settings::Canvas& c,int x,int y,int w,int h,int r,unsigned fill,unsigned edge){
  for(int j=0;j<h;++j)for(int i=0;i<w;++i){
    const int cxp=i<r?r:i>=w-r?w-r-1:i, cyp=j<r?r:j>=h-r?h-r-1:j;   // nearest point of the inner (unrounded) rectangle
    float a=1.f,inner=1.f;
    if((i<r||i>=w-r)&&(j<r||j>=h-r)){
      const float dx=float(i-cxp),dy=float(j-cyp);const float d2=dx*dx+dy*dy;
      float q=d2,sq=q>0?q*0.5f+0.5f:0;if(q>0)for(int k=0;k<4;++k)sq=0.5f*(sq+q/sq);
      const float o=sq-(r-0.5f);a=o<=-0.5f?1.f:o>=0.5f?0.f:0.5f-o;
      const float oi=sq-(r-1.5f);inner=oi<=-0.5f?1.f:oi>=0.5f?0.f:0.5f-oi;
    }else if(i==0||j==0||i==w-1||j==h-1)inner=0.f;
    if(a<=0)continue;
    blendPx(c,x+i,y+j,mix(edge,fill,unsigned(inner*255.f+0.5f)),unsigned(a*255.f+0.5f));
  }
}

// Greedy word wrap into at most two lines of `width` pixels; returns the line count.
inline int wrap2(const citro_settings::Canvas& c,const char* s,int width,char out[2][40]){
  out[0][0]=out[1][0]=0;int line=0;
  for(const char* p=s;*p&&line<2;){
    while(*p==' ')++p;
    const char* e=p;while(*e&&*e!=' ')++e;
    char next[40];std::snprintf(next,sizeof next,"%s%s%.*s",out[line],out[line][0]?" ":"",int(e-p),p);
    if(inkWidth(c,next)<=width||!out[line][0]){std::snprintf(out[line],40,"%s",next);p=e;}
    else ++line;
  }
  return out[1][0]?2:out[0][0]?1:0;
}

inline void button(citro_settings::Canvas& c,n3ds_ui::Rect r,bool enabled,bool pressed){
  const unsigned fill=pressed?0x2f8790:enabled?0x1d4f5c:0x142f39, edge=pressed?0xffe7a0:enabled?0xc9b673:0x35545c;
  roundRect(c,r.x,r.y,r.w,r.h,4,fill,edge);
  if(!pressed)c.rect(r.x+4,r.y+r.h-3,r.w-8,1,mix(fill,0x000000,60));   // soft lower lip
}

inline void badge(citro_settings::Canvas& c,int cx,int cy,const char* key,bool enabled){
  aaDisk(c,cx+0.5f,cy+0.5f,9.5f,enabled?0x27606b:0x1b3640,enabled?0xe0cf8c:0x4c6469);
  centerMid(c,cx-9,cy,20,key,enabled?0xfff4cf:0x8aa0a3);
}

// The button column (x >= kGpuRows). Pixels of the left area are left alone.
inline void composeColumn(citro_settings::Canvas& c,const State& s){
  using namespace n3ds_ui;
  const Presentation pr{s.presentation};
  const bool visible=pr.alpha()!=0;
  c.rect(kGpuRows,0,320-kGpuRows,240,0x0e2530);c.rect(kGpuRows,0,1,240,0x30535d);
  if(!visible){if(s.fps)centerMid(c,kGpuRows,230,320-kGpuRows,s.fpsText,0xffe6a3);return;}
  static const char* keys[3]={"X","Y","Z"};
  for(int i=0;i<5;++i){
    const Rect r=targets[i].rect;const uint32_t action=targets[i].action;
    const bool drawn=(pr.drawn()&action)!=0;
    uint8_t id=255;if(i<3)id=uint8_t(s.items>>(8*i));
    const bool has=i>=3||id!=255;
    const bool enabled=drawn&&has&&(pr.usable()&action)!=0;
    const bool pressed=enabled&&s.pressedSlot==i;
    button(c,r,enabled,pressed);
    const int dy=pressed?1:0, cy=r.y+r.h/2+dy;
    if(i<3){
      badge(c,r.x+14,cy-1,keys[i],enabled);
      const int tx=r.x+29,tw=r.x+r.w-5-tx;
      if(!has){textMid(c,tx,cy-1,"Empty",0x4d6569,tw);continue;}
      char lines[2][40];const int n=wrap2(c,itemName(id),tw,lines);
      const unsigned col=enabled?0xfdf3d2:0x7d9397;
      if(n==1)textMid(c,tx,cy-1,lines[0],col,tw);
      else {textMid(c,tx,cy-8,lines[0],col,tw);textMid(c,tx,cy+7,lines[1],col,tw);}
    }else{
      centerMid(c,r.x,cy-1,r.w,i==3?"Menu":"Map",enabled?0xffe6a4:0x7d9397);
    }
  }
  if(s.fps)centerMid(c,kGpuRows,230,320-kGpuRows,s.fpsText,0xffe6a3);
}

// A key cap: rounded box around a label, centred on cy; returns its width.
inline int keycap(citro_settings::Canvas& c,int x,int cy,const char* label){
  const int w=std::max(22,inkWidth(c,label)+14),h=22;
  roundRect(c,x,cy-h/2,w,h,4,0x1d4f5c,0xc9b673);
  centerMid(c,x,cy-1,w,label,0xfff4cf);
  return w;
}

// The left area for the CPU panel (no live map).
// (Claude v139/v140) header with an accent rule, a control legend of key caps, one hint line, Settings button; every label centred on its ink.
inline void composeLeft(citro_settings::Canvas& c,const uint16_t* cloth,const State& s){
  for(int x=0;x<kGpuRows;++x)std::memcpy(c.pixels+x*240,cloth+x*240,240*2);
  constexpr int L=12,W=192;   // card column: x 12..203, centred in the 216-pixel left area
  // header
  roundRect(c,L,8,W,38,5,0x0d2936,0x547c85);
  centerMid(c,L,22,W,"CitroWaker",0xffe3a1);
  c.rect(L+28,36,W-56,1,0x30535d);
  for(int i=0;i<5;++i){const int h=i<3?i:4-i;c.rect(L+W/2-h,34+i,2*h+1,1,0xc9b673);}
  // control legend
  roundRect(c,L,54,W,136,5,0x0d2936,0x30535d);
  textMid(c,L+12,68,"Controls",0x9dc1c6);
  c.rect(L+12,80,W-24,1,0x24434c);
  const unsigned label=0xd7e6e6,dim=0x8fb0b4;
  constexpr int kDesc=L+96;
  {int x=L+12;for(const char* k:{"X","Y","Z"})x+=keycap(c,x,98,k)+4;textMid(c,kDesc,97,"Use item",label,W-96-8);}
  keycap(c,L+12,126,"Menu");textMid(c,kDesc,125,"Pause menu",label,W-96-8);
  keycap(c,L+12,154,"Map");textMid(c,kDesc,153,"Map size",label,W-96-8);
  centerMid(c,L,177,W,"Live map: Settings",dim);
  // settings
  const bool enabled=true;   // the settings menu pauses the game itself: usable in any game state
  button(c,kSettingsButton,enabled,s.settingsPressed);
  centerMid(c,kSettingsButton.x,kSettingsButton.y+kSettingsButton.h/2-1+(s.settingsPressed?1:0),kSettingsButton.w,"Settings  (START+SELECT)",enabled?0xffe6a4:0x7d9397);
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
