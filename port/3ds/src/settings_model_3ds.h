#pragma once
#include <array>
#include <string>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <unistd.h>
#include "performance_3ds.h"
namespace citro_settings {
struct Row {const char* label;const char* key;const char* help;int choices;};
inline constexpr Row rows[]={
 {"Screen layout","widescreen","Restart to change the top screen layout.",2},
 {"Bottom screen","bottom_ui","Panel or live map. Applies when you return.",3},
 {"HUD layout","top_hud","Fit the game prompts to the 3DS screen.",2},
 {"Frame counter","frame_counter","Live counter on the bottom panel.",2},
 {"Texture source","texture_pack","PC modes need a compatible prepared pack.",3},
 {"Geometry reuse","retain_geometry","Reuse unchanged geometry. Restart required.",2},
 {"Draw command cache","draw_replay","Reuse validated GPU commands. Restart required.",2},
 {"Incremental plans","plan_delta","Reuse matching draw plans. Restart required.",2},
 {"Render worker","core2","Use the render worker. Restart required.",2},
 {"GX worker","fifo_thread","Use the GX worker. Restart required.",2},
 {"Offscreen mesh test","rigid_clip","Experimental. Reject fully offscreen meshes.",2},
 {"Fractional pacing","paint_fractional","Experimental. Use measured frame costs.",2},
 {"Staging pipeline","pipeline_stage8","Overlap recording and rendering. Restart.",2},
 {"Audio on core 1","audio_core1","Audio thread placement. Restart required.",2},
};
inline constexpr int starts[]={0,5,10,14};
struct Model {
 std::array<int,14> value{};int page=0,selected=0;bool saved=false;
 Model(const Performance3ds& p){value={p.widescreen,p.bottomUi?(p.uiMapBottom?2:1):0,p.widescreen&&p.topHud,p.frameCounter,p.texturePack?(p.textureShards?2:1):0,p.retainGeometry,p.drawReplay,p.planDelta,p.core2!=0,p.fifoThread,p.rigidClip,p.paintFractional,p.pipelineStage8,p.audioCore1};}
 int index()const{return starts[page]+selected;}
 int count()const{return starts[page+1]-starts[page];}
 void tab(int delta){page=(page+delta+3)%3;selected=0;}
 void move(int delta){selected=(selected+delta+count())%count();}
 void cycle(int delta){int i=index();if(i==2&&!value[0])return;value[i]=(value[i]+delta+rows[i].choices)%rows[i].choices;if(!value[0])value[2]=0;}
 const char* help()const{if(index()==2&&!value[0])return "Full width enables the 3DS HUD layout.";if(index()==4)return value[4]==0?"Converts textures locally; current default.":value[4]==1?"Legacy pack may be slower than runtime.":"Experimental split pack. Restart to test.";return rows[index()].help;}
 const char* text(int i)const{if(i==0)return value[i]?"Full width":"Original 4:3";if(i==1)return value[i]==2?"Live map":value[i]?"Panel":"Off";if(i==2)return value[i]?"3DS fit":"Original";if(i==4)return value[i]==0?"Runtime":value[i]==1?"PC single file":"PC split files";return value[i]?"On":"Off";}
 std::string patch(const std::string& old)const{
  std::array<bool,16> seen{};std::string out;size_t at=0;
  while(at<old.size()){
   size_t end=old.find('\n',at);if(end==std::string::npos)end=old.size();else ++end;
   std::string line=old.substr(at,end-at);at=end;
   size_t first=line.find_first_not_of(" \t\r\n");size_t equal=line.find('=');int found=-1;
   if(first!=std::string::npos&&line[first]!='#'&&line[first]!=';'&&equal!=std::string::npos){
    std::string key=line.substr(first,equal-first);while(!key.empty()&&std::isspace((unsigned char)key.back()))key.pop_back();
    for(int i=0;i<14;++i)if(key==rows[i].key){found=i;break;}
    if(key=="texture_shards")found=14;
    if(key=="ui_map_bottom")found=15;
   }
   if(found<0){out+=line;continue;}
   int v=found==15?int(value[1]==2):found==14?int(value[4]==2):found==4?int(value[4]!=0):found==1?int(value[1]!=0):value[found];
   const char* key=found==15?"ui_map_bottom":found==14?"texture_shards":rows[found].key;
   out+=std::string(key)+"="+std::to_string(v);
   size_t comment=line.find('#',equal+1);if(comment!=std::string::npos){out+=' ';out+=line.substr(comment);if(out.back()!='\n')out+='\n';}else out+='\n';seen[found]=true;
  }
  if(!out.empty()&&out.back()!='\n')out+='\n';
  for(int i=0;i<16;++i)if(!seen[i]){int v=i==15?int(value[1]==2):i==14?int(value[4]==2):i==4?int(value[4]!=0):i==1?int(value[1]!=0):value[i];out+=std::string(i==15?"ui_map_bottom":i==14?"texture_shards":rows[i].key)+"="+std::to_string(v)+"\n";}
  return out;
 }
 bool save(const char* path)const{
  const std::string backupPath=std::string(path)+".ui.bak";
  std::string old;char buf[1024];FILE* f=std::fopen(path,"rb");if(!f&&errno==ENOENT)f=std::fopen(backupPath.c_str(),"rb");if(f){while(size_t n=std::fread(buf,1,sizeof buf,f)){old.append(buf,n);if(old.size()>65536){std::fclose(f);return false;}}bool bad=std::ferror(f);std::fclose(f);if(bad)return false;}else if(errno!=ENOENT)return false;
  const auto content=patch(old);const std::string tmp=std::string(path)+".ui.tmp",bak=std::string(path)+".ui.bak";
  f=std::fopen(tmp.c_str(),"wb");if(!f)return false;
  bool ok=std::fwrite(content.data(),1,content.size(),f)==content.size()&&std::fflush(f)==0;
  if(ok&&::fsync(fileno(f))!=0&&errno!=ENOSYS)ok=false;
  if(std::fclose(f)!=0)ok=false;
  if(!ok){std::remove(tmp.c_str());return false;}
  if(std::rename(tmp.c_str(),path)==0){std::remove(bak.c_str());return true;}
  // The 3DS SD archive refuses to rename onto an existing file, and libctru then
  // retries as a directory rename and reports ENOENT (console v132 log: "save
  // failed errno 2"). Move the old file aside whatever errno says, put the new
  // one in place, and keep the backup until that worked (startup reads
  // performance.ini.ui.bak when performance.ini is missing).
  if(std::remove(bak.c_str())!=0&&errno!=ENOENT){std::remove(tmp.c_str());return false;}
  const bool moved=std::rename(path,bak.c_str())==0;
  if(!moved){if(FILE* still=std::fopen(path,"rb")){std::fclose(still);std::remove(tmp.c_str());return false;}}
  if(std::rename(tmp.c_str(),path)!=0){if(moved)std::rename(bak.c_str(),path);std::remove(tmp.c_str());return false;}
  std::remove(bak.c_str());return true;
 }
};
}
