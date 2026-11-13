// Optional startup-only QoL profile. Original behaviour when absent/invalid.
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdlib>
namespace n3ds_qol {
struct Profile {const char* sail="0";bool grapple=false,songs=false,text=false,gyro=false,gyroInvertX=false,gyroInvertY=false;int gyroSensitivity=100;};
inline char* trim(char* p){while(*p==' '||*p=='\t')++p;char* e=p+std::strlen(p);while(e>p&&(e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n'))--e;*e=0;return p;}
inline int load(const char* path,Profile& out){
 FILE* f=std::fopen(path,"r");if(!f)return -1;
 Profile next;char line[160];unsigned seen=0;int number=0,result=0;
 while(std::fgets(line,sizeof line,f)){
  ++number;if(number>64||(!std::strchr(line,'\n')&&!std::feof(f))){result=number;break;}
  char* p=trim(line);if(!*p||*p=='#'||*p==';')continue;
  char* eq=std::strchr(p,'=');if(!eq){result=number;break;}*eq++=0;
  char* key=trim(p);char* value=trim(eq);unsigned bit=0;
  if(!std::strcmp(key,"sail")){
   bit=1;
   if(!std::strcmp(value,"original"))next.sail="0";
   else if(!std::strcmp(value,"swift"))next.sail="swift";
   else if(!std::strcmp(value,"wind"))next.sail="wind";
   else {result=number;break;}
  }else if(!std::strcmp(key,"gyro_sensitivity")){
   bit=32;char* end=nullptr;const long v=std::strtol(value,&end,10);
   if(!*value||*end||v<25||v>300){result=number;break;}next.gyroSensitivity=int(v);
  }else{
   bool* dest=nullptr;
   if(!std::strcmp(key,"gyro_invert_x")){bit=64;dest=&next.gyroInvertX;}
   else if(!std::strcmp(key,"gyro_invert_y")){bit=128;dest=&next.gyroInvertY;}
   else if(!std::strcmp(key,"gyro")){bit=16;dest=&next.gyro;}
   else if(!std::strcmp(key,"fast_grapple")){bit=2;dest=&next.grapple;}
   else if(!std::strcmp(key,"no_song_replay")){bit=4;dest=&next.songs;}
   else if(!std::strcmp(key,"instant_text")){bit=8;dest=&next.text;}
   if(!dest||(std::strcmp(value,"0")&&std::strcmp(value,"1"))){result=number;break;}
   *dest=*value=='1';
  }
  if(seen&bit){result=number;break;}seen|=bit;
 }
 if(std::ferror(f)&&!result)result=number?number:1;
 std::fclose(f);if(!result)out=next;return result;
}
}
