#pragma once
#include "prepared_commands.h"
namespace t2p {
// Versioned producer/consumer contract, changed whenever this command recipe
// changes. Inputs: enabled texture units, index offset, index count. Addresses
// are bound on the console, never baked into the PC artifact.
inline constexpr uint64_t preparedDrawFingerprint=0x4357445241570001ull;
inline PreparedCommands compilePreparedDraw(){
 PreparedCommands p;p.fingerprint=preparedDrawFingerprint;
 const char key[]="citrowaker/pica/indexed-triangles16/v1";p.key.assign(key,key+sizeof(key)-1);
 auto write=[&](uint32_t reg,uint32_t mask,uint32_t value){p.words.push_back(value);p.words.push_back(reg|(mask<<16));};
 write(0x80,1,0);p.patches.push_back({0,0,7});
 write(0x25e,2,0x300);write(0x25f,15,1);
 const auto index=uint32_t(p.words.size());write(0x227,15,0x80000000u);p.patches.push_back({index,1,0x7fffffffu});
 const auto count=uint32_t(p.words.size());write(0x228,15,0);p.patches.push_back({count,2,0xffffffffu});
 write(0x22a,15,0);write(0x229,2,0x100);write(0x253,2,0x100);
 write(0x245,1,0);write(0x22f,15,1);write(0x245,1,1);
 write(0x229,2,0);write(0x253,2,0);write(0x231,15,1);
 write(0x25e,8,0);write(0x25e,8,0);
 p.offsets[8]=uint32_t(p.words.size());return p;
}
inline bool compatiblePreparedDraw(const PreparedCommands& p){
 const auto expected=compilePreparedDraw();
 if(p.fingerprint!=expected.fingerprint || p.key!=expected.key || p.offsets!=expected.offsets || p.words!=expected.words || p.patches.size()!=expected.patches.size())return false;
 for(size_t i=0;i<p.patches.size();++i){const auto& a=p.patches[i];const auto& b=expected.patches[i];if(a.word!=b.word||a.input!=b.input||a.mask!=b.mask)return false;}
 return true;
}
}
