#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace t2p {
// Immutable geometry, memoized only by the render owner. No per-frame allocation.
struct ClipBounds {
  float lo[3]={},hi[3]={};
  int state=0; // 0 unknown,1 finite bounds,-1 not eligible
};
struct RigidClipPlanes {
  float plane[4][4]={},error[4][4]={},w[4]={},werror[4]={};
  float keyM[3][4]={},keyP[4][4]={};bool valid=false;
  // Loose bounds for both shader dot products, uniform/input rounding included.
  // Includes CPUfloat32 coefficient/bound rounding, with a wide reserve over
  // shader arithmetic rounded to16fractional bits.
  static constexpr float roundoff=1.0f/1024.0f,absolute=1e-6f;
  // Bounded inputs also keep intermediate shader products below float24 overflow.
  bool update(const float m[3][4],const float p[4][4]) {
    if(valid&&!std::memcmp(keyM,m,sizeof keyM)&&!std::memcmp(keyP,p,sizeof keyP))return true;
    valid=false;
    for(int j=0;j<3;++j)for(int k=0;k<4;++k)if(!std::isfinite(m[j][k])||std::fabs(m[j][k])>1e6f)return false;
    for(int j=0;j<4;++j)for(int k=0;k<4;++k)if(!std::isfinite(p[j][k])||std::fabs(p[j][k])>1e6f)return false;
    const float gamma=2*roundoff+roundoff*roundoff;
    for(int k=0;k<4;++k){
      w[k]=k==3?p[3][3]:0;werror[k]=0;
      for(int j=0;j<3;++j){w[k]+=float(p[3][j])*m[j][k];werror[k]+=gamma*std::fabs(float(p[3][j])*m[j][k]);}
    }
    float pw=0;for(int j=0;j<3;++j)pw+=std::fabs(p[3][j]);
    werror[3]+=roundoff*std::fabs(p[3][3])+(1+roundoff)*absolute*pw+absolute;
    for(int side=0;side<4;++side){
      const int row=side/2;const float sign=side%2?-1:1;
      for(int k=0;k<4;++k){
        plane[side][k]=k==3?sign*p[row][3]+p[3][3]:0;error[side][k]=0;
        for(int j=0;j<3;++j){
          plane[side][k]+=(sign*p[row][j]+p[3][j])*float(m[j][k]);
          error[side][k]+=gamma*(std::fabs(p[row][j])+std::fabs(p[3][j]))*std::fabs(m[j][k]);
        }
      }
      float weights=0;for(int j=0;j<3;++j)weights+=std::fabs(p[row][j])+std::fabs(p[3][j]);
      error[side][3]+=roundoff*(std::fabs(p[row][3])+std::fabs(p[3][3]))+(1+roundoff)*absolute*weights+2*absolute;
    }
    std::memcpy(keyM,m,sizeof keyM);std::memcpy(keyP,p,sizeof keyP);valid=true;return true;
  }
  unsigned outside(const ClipBounds& b)const {
    if(!valid||b.state!=1)return 0;
    float mx[3];for(int k=0;k<3;++k){if(!std::isfinite(b.lo[k])||!std::isfinite(b.hi[k])||b.lo[k]>b.hi[k])return 0;mx[k]=std::max(std::fabs(b.lo[k]),std::fabs(b.hi[k]));if(mx[k]>1e6f)return 0;}
    float lowW=w[3],highW=w[3],errW=werror[3];
    for(int k=0;k<3;++k){lowW+=w[k]*(w[k]>=0?b.lo[k]:b.hi[k]);highW+=w[k]*(w[k]>=0?b.hi[k]:b.lo[k]);errW+=werror[k]*mx[k];}
    // Near/behind-camera, extreme values and ambiguous cases always render.
    if(lowW<=errW+absolute||highW+errW>1e12f)return 0;
    unsigned out=0;
    for(int side=0;side<4;++side){
      float hi=plane[side][3],err=error[side][3],magnitude=std::fabs(plane[side][3]);
      for(int k=0;k<3;++k){hi+=plane[side][k]*(plane[side][k]>=0?b.hi[k]:b.lo[k]);err+=error[side][k]*mx[k];magnitude+=std::fabs(plane[side][k])*mx[k];}
      if(!std::isfinite(magnitude)||magnitude+err>1e12f)return 0;
      // An extra1% of clip-w keeps a several-pixel guard beyond the screen edge.
      if(std::isfinite(hi)&&std::isfinite(err)&&hi+err+0.01f*(highW+errW)<0)out|=1u<<side;
    }
    return out;
  }
};
}
