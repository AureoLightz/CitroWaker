#pragma once
#include "settings_canvas_3ds.h"
#include "settings_model_3ds.h"
namespace citro_settings {
inline void draw(Canvas c,const Model& m,bool error=false){
 c.background();c.text(16,12,"CitroWaker",0xa2c9cc);c.text(213,12,"Settings",0xffe6a4);
 const char* tabs[]={"Display","Performance","Experimental"};const int widths[]={85,108,107};int x=10;
 for(int p=0;p<3;++p){c.rect(x,36,widths[p]-2,23,p==m.page?0x226d78:0x173e4b);c.frame(x,36,widths[p]-2,23,p==m.page?0xdbc981:0x3c6d78);c.center(x,39,widths[p]-2,tabs[p],p==m.page?0xfff3c7:0xb3cccf);x+=widths[p];}
 for(int row=0;row<m.count();++row){const int i=starts[m.page]+row,y=65+row*23;const bool focus=row==m.selected;
  c.rect(12,y,296,22,focus?0x276d79:0x173b49);c.frame(12,y,296,22,focus?0xf2d583:0x315866);
  c.text(20,y+3,rows[i].label,focus?0xfff4cf:0xd1e0df,173);
  c.rect(197,y+3,103,16,focus?0x123f4d:0x112d3a);c.center(197,y+3,103,m.text(i),focus?0xffe699:0x9fcbca);
 }
 c.rect(12,181,296,30,0x0d2936);
 // Two-line word wrapping, clipped to the framed help area.
 const char* help=error?"Save failed. X returns without saving.":m.saved?"Display applied. Restart for layout, textures or worker changes.":m.help();char line[128]={};int used=0,y=181;
 for(const char* p=help;;){const char* end=p;while(*end&&*end!=' ')++end;const int n=int(end-p);char next[128];std::snprintf(next,sizeof next,"%s%s%.*s",line,used?" ":"",n,p);
  if(c.width(next)>280&&used){c.text(18,y,line,error?0xffb5a6:0x9cc3c9,284);y+=13;std::snprintf(line,sizeof line,"%.*s",n,p);}else std::snprintf(line,sizeof line,"%s",next);used=int(std::strlen(line));
  if(!*end){if(y<208)c.text(18,y,line,error?0xffb5a6:0x9cc3c9,284);break;}p=end+1;
 }
 c.rect(12,213,123,19,0x234959);c.frame(12,213,123,19,0x6b999e);c.disk(32,222,7,0xa5c3c2);c.disk(32,222,6,0x254a59);c.center(25,215,14,"B",0xf5edcb);c.text(48,215,"Back",0xd2e2df);
 c.rect(141,213,167,19,0x276b70);c.frame(141,213,167,19,0xd6c27b);c.disk(156,222,7,0xdfcf8a);c.disk(156,222,6,0x285d65);c.center(149,215,14,"Y",0xfff3cd);c.text(169,215,"Apply and return",0xffedb2);
}
}
