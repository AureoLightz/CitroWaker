// Immutable producer snapshot: recorder must carry this word with its frame.
#pragma once
#include <stdint.h>
namespace n3ds_ui {
enum UiBlock : uint32_t {Cinematic=1,Pause=2,Transition=4,Unavailable=8};
struct Presentation {
 uint32_t word=0;
 uint8_t alpha() const {return uint8_t(word);}
 uint32_t drawn() const {return (word>>8)&31u;}
 uint32_t usable() const {return (word>>13)&31u;}
 uint32_t blocked() const {return (word>>18)&15u;}
};
// No independent fade timer: the game owns HUD opacity and simulation timing.
inline Presentation presentation(uint8_t alpha,uint32_t drawn,uint32_t usable,uint32_t blocked){
 blocked&=15u;
 if(blocked)alpha=0;
 drawn=alpha?drawn&31u:0;
 // A fading/hidden panel cannot accept a tap against an old visible frame.
 usable=alpha==255&&!blocked?usable&drawn&31u:0;
 return {uint32_t(alpha)|(drawn<<8)|(usable<<13)|(blocked<<18)};
}
}
