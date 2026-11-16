#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include "settings_font_3ds.h"
namespace citro_settings {
struct Canvas {
 uint16_t* pixels;
 static uint16_t rgb(unsigned c){return uint16_t(((c>>19)&31)<<11|((c>>10)&63)<<5|((c>>3)&31));}
 void put(int x,int y,unsigned c){if(x>=0&&x<320&&y>=0&&y<240)pixels[x*240+239-y]=rgb(c);}
 void rect(int x,int y,int w,int h,unsigned c){for(int i=std::max(0,x);i<std::min(320,x+w);++i)for(int j=std::max(0,y);j<std::min(240,y+h);++j)put(i,j,c);}
 void disk(int x,int y,int r,unsigned colour){for(int j=-r;j<=r;++j)for(int i=-r;i<=r;++i)if(i*i+j*j<=r*r)put(x+i,y+j,colour);}
 void frame(int x,int y,int w,int h,unsigned c){rect(x,y,w,1,c);rect(x,y+h-1,w,1,c);rect(x,y,1,h,c);rect(x+w-1,y,1,h,c);}
 int width(const char* s)const{int n=0;for(;*s;++s){unsigned c=unsigned(uint8_t(*s));if(c>=32&&c<127)n+=kSettingsAdvance[c-32];}return n;}
 void text(int x,int y,const char* s,unsigned colour,int maxWidth=320){
  int end=x+maxWidth;
  for(;*s;++s){const unsigned c=unsigned(uint8_t(*s));if(c<32||c>=127)continue;const unsigned glyph=c-32;int a=kSettingsAdvance[glyph];if(x+a>end)break;
   for(int j=0;j<18&&y+j<240;++j)for(int i=0;i<14&&x+i<end;++i){const unsigned alpha=kSettingsGlyph[glyph*14*18+j*14+i];if(!alpha||x+i<0||x+i>=320||y+j<0)continue;const uint16_t old=pixels[(x+i)*240+239-y-j];const unsigned r=((old>>11)*255+15)/31,g=(((old>>5)&63)*255+31)/63,b=((old&31)*255+15)/31;
    const unsigned nr=(((colour>>16)&255)*alpha+r*(255-alpha)+127)/255,ng=(((colour>>8)&255)*alpha+g*(255-alpha)+127)/255,nb=((colour&255)*alpha+b*(255-alpha)+127)/255;put(x+i,y+j,(nr<<16)|(ng<<8)|nb);
   }x+=a;
  }
 }
 void center(int x,int y,int w,const char* s,unsigned c){text(x+(w-width(s))/2,y,s,c,w);}
 void background(){for(int x=0;x<320;++x)for(int y=0;y<240;++y){unsigned n=((x*29+y*47)^(x*y))&3;put(x,y,((14+n)<<16)|((31+n)<<8)|(44+n));}frame(5,5,310,230,0x659798);frame(7,7,306,226,0x254b59);}
};
}
