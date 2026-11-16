// libctru side of the PAD sensor bridge. Included in exactly one devkitARM TU.
#pragma once
#include <3ds.h>
#include <cmath>
namespace n3ds_motion {
inline bool enabled[3]={false,false,false},attempted[3]={false,false,false};
inline bool gyroCoefficientRead=false;
inline float gyroRadiansPerRaw=0;
inline bool valid(int sensor){return sensor==1||sensor==2;}
}
extern "C" int n3dsMotionEnable(int sensor,int on){
 using namespace n3ds_motion;
 if(!valid(sensor))return 0;
 on=on!=0;
 if(!on&&!enabled[sensor]){attempted[sensor]=false;return 1;}
 if(on&&enabled[sensor])return 1;
 if(on&&attempted[sensor])return 0;
 if(on)attempted[sensor]=true;
 if(on&&sensor==2&&!gyroCoefficientRead){
  float coefficient=0;
  const Result r=HIDUSER_GetGyroscopeRawToDpsCoefficient(&coefficient);
  if(R_FAILED(r)||!std::isfinite(coefficient)||coefficient<=0)return 0;
  // Service coefficient is raw units per degree/second (typically14.375).
  gyroRadiansPerRaw=0.017453292519943295f/coefficient;gyroCoefficientRead=true;
 }
 Result r=sensor==1?(on?HIDUSER_EnableAccelerometer():HIDUSER_DisableAccelerometer()):
                     (on?HIDUSER_EnableGyroscope():HIDUSER_DisableGyroscope());
 if(R_FAILED(r))return 0;enabled[sensor]=bool(on);if(!on)attempted[sensor]=false;return 1;
}
extern "C" int n3dsMotionRead(int sensor,float* out,int count){
 using namespace n3ds_motion;
 if(!out||count<3||!valid(sensor)||!enabled[sensor])return 0;
 // PADRead already calls hidScanInput. Do NOT scan again or create a thread.
 if(sensor==1){accelVector v{};hidAccelRead(&v);constexpr float g=9.80665f/512.0f;
  out[0]=v.x*g;out[1]=v.y*g;out[2]=v.z*g;
 }else{angularRate v{};hidGyroRead(&v);
  out[0]=v.x*gyroRadiansPerRaw;out[1]=v.y*gyroRadiansPerRaw;out[2]=v.z*gyroRadiansPerRaw;
 }
 return 1;
}
extern "C" void n3dsMotionStop(void){n3dsMotionEnable(2,0);n3dsMotionEnable(1,0);}
