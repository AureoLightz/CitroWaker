#pragma once
#include <algorithm>
#include <cmath>
#include "picaenc.h"
#include "pipeline.h"
// Recover only the exact far-depth sentinel. Other numeric GX depth peeks are
// deliberately unsupported; W depth cannot generally identify its writer.
struct DepthVisibility3ds {
  struct Camera {float a,b,nearPlane,farPlane,nearZ,farZ;bool ortho;};
  Camera camera[16]{};unsigned count=0;bool complete=true;
  float fogRange=0;
  void add(Camera c){
    for(unsigned i=0;i<count;i++){const auto& x=camera[i];if(x.a==c.a&&x.b==c.b&&x.nearPlane==c.nearPlane&&x.farPlane==c.farPlane&&x.nearZ==c.nearZ&&x.farZ==c.farZ&&x.ortho==c.ortho)return;}
    if(count==16){complete=false;return;}camera[count++]=c;
  }
  bool isFar(uint32_t raw)const{
    raw&=0xFFFFFFu;
    if(t2p::picaReverseZ())return raw<=0x100u;   // (Claude v107) reversed Z: clears write 0, geometry at d stores ~kPicaDepthMax*n/d (sky at the far plane reads a few codes)
    // (Claude v84) far copy-clears write kPicaDepthClearFar (1023/1024 = 0xFFBFFF) and frame clears 0xFFFFFF; geometry ends at 255/256 (0xFEFFFF) plus
    // the console's W error, well below this threshold.
    if(raw>=0xFFBBFFu)return true;
    if(!complete||!(fogRange>0))return false;
    constexpr double maximum=16777215.0;
    bool compatible=false;
    for(unsigned i=0;i<count;i++){
      const auto& c=camera[i];
      if(!(c.farZ>0)||!std::isfinite(c.a)||!std::isfinite(c.b))return false;
      const double scale=t2p::picaF24toF32(t2p::picaF32toF24(c.farZ*(255.0f/256.0f)/fogRange));   // (Claude v84) W map offset kPicaDepthMax / D
      if(!(scale>0))return false;
      double lo=std::max(0.0,(double(raw)-1.0)/maximum)/scale;
      double hi=(double(raw)+1.0)/maximum/scale;
      if(c.ortho){if(lo<=1&&hi>=1)return false;continue;}
      if(!(c.nearPlane>0&&c.farPlane>c.nearPlane&&c.b<0&&c.farZ>c.nearZ))return false;
      // Include a small clip-boundary tolerance for f24 vertex arithmetic.
      const double tolerance=double(c.farPlane)*0.0001;
      if(hi<c.nearPlane-tolerance||lo>c.farPlane+tolerance)continue;
      compatible=true;
      // (Claude v82) GX z grows with w, so test the largest w the value allows. The console's W-buffer is less precise than Azahar's (f24 w and
      // interpolation, ~2^-13 relative): the sky behind the sun reads a few dozen codes below the emulator's 0xFFFFE3 and the sun was judged hidden
      // (no glare on hardware). Allow that much: anything that really covers the sun (cliffs, ship, sails) is far nearer than 0.02% of the far plane.
      const double whi=std::min(std::max(hi*(1.0+1.0/4096.0),double(c.nearPlane)),double(c.farPlane));
      const double gx=c.nearZ+(c.farZ-c.nearZ)*(1.0-c.a+c.b/whi);
      if(!(gx>=1.0-0.5/maximum))return false;
    }
    return compatible;
  }
};
