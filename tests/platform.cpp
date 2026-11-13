#include "qol_config_3ds.h"
#include "ui_shortcut_config_3ds.h"
#include "ui_lifecycle_3ds.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static FILE* config(const char* text) {
    FILE* f=std::tmpfile(); assert(f);
    std::fwrite(text,1,std::strlen(text),f);std::rewind(f);return f;
}
int main(int argc,char** argv) {
    assert(argc==2);
    using namespace n3ds_ui;
    unsigned checked=0;
    for(unsigned alpha=0;alpha<256;alpha++)
    for(unsigned drawn=0;drawn<32;drawn++)
    for(unsigned usable=0;usable<32;usable++)
    for(unsigned block=0;block<16;block++) {
        auto p=presentation(alpha,drawn,usable,block);
        assert(p.alpha()==(block?0:alpha));
        assert(p.drawn()==(!block&&alpha?drawn:0));
        assert(p.usable()==(!block&&alpha==255?(drawn&usable):0));
        assert(p.blocked()==block);++checked;
    }
    ShortcutConfig profile;
    FILE* f=config("slot1.action = Y\nslot2.action = X\nslot3.action = NONE\n");
    assert(profile.read(f)==0);std::fclose(f);
    assert(profile.slots[0].action==N3DS_UI_ITEM_Y);
    assert(profile.slots[1].action==N3DS_UI_ITEM_X);
    assert(profile.slots[2].action==0);
    for(const char* bad:{"slot4.action=X\n","slot1.x=0\n","slot1.action=MENU\n","slot9.action=X\n"}) {
        f=config(bad);assert(profile.read(f)>0);std::fclose(f);
        assert(profile.slots[0].action==N3DS_UI_ITEM_Y); // rejection is transactional
    }
    f=config("slot1.action=X\nslot2.action=X\n");
    assert(profile.read(f)==0);std::fclose(f);
    Touch touch;
    assert(touch.update(true,250,30,allActions,false,profile.slots)==N3DS_UI_ITEM_X);
    assert(touch.pressedSlot()==0);
    assert(touch.update(true,250,90,allActions,false,profile.slots)==0); // drag cancels
    assert(touch.update(true,250,30,allActions,false,profile.slots)==0); // cannot recapture until release
    touch.update(false,0,0,allActions,false,profile.slots);
    assert(touch.update(true,250,90,allActions,false,profile.slots)==N3DS_UI_ITEM_X);
    assert(touch.pressedSlot()==1); // duplicate assignment highlights the touched slot only
    touch.invalidate(N3DS_UI_ITEM_X);
    assert(touch.update(true,250,90,allActions,false,profile.slots)==0);
    touch.update(false,0,0,0);
    assert(touch.update(true,20,210,allActions,true)==0); // cutscene/pause block
    assert(hit(319,239,allActions)==0);
    assert(actionsForItems(allActions,0xffffff)==(N3DS_UI_MENU|N3DS_UI_MAP_SIZE));
    assert(actionsForItems(allActions,0xffff01)&N3DS_UI_ITEM_X);

    n3ds_qol::Profile qos;
    auto load=[&](const std::string& text) {
        FILE* file=std::fopen(argv[1],"w");assert(file);
        std::fwrite(text.data(),1,text.size(),file);std::fclose(file);
        return n3ds_qol::load(argv[1],qos);
    };
    for(const char* sail:{"original","swift","wind"})
    for(unsigned mask=0;mask<16;mask++) {
        std::string text=std::string("sail=")+sail+"\n";
        const char* names[]={"fast_grapple","no_song_replay","instant_text","gyro"};
        for(unsigned i=0;i<4;i++)text+=std::string(names[i])+"="+char('0'+((mask>>i)&1))+"\n";
        assert(load(text)==0);
        assert(qos.grapple==bool(mask&1)&&qos.songs==bool(mask&2)&&qos.text==bool(mask&4)&&qos.gyro==bool(mask&8));
    }
    assert(load("sail=swift\nfast_grapple=1\n")==0);
    for(const char* bad:{"unknown=1\n","sail=wind\nsail=original\n","gyro=2\n","gyro_sensitivity=24\n","gyro_sensitivity=301\n","gyro_sensitivity=nan\n"}) {
        assert(load(bad)>0);assert(!std::strcmp(qos.sail,"swift")&&qos.grapple);
    }
    assert(load(std::string(300,'x'))>0);
    for(int sensitivity:{25,100,300}) {
        assert(load("gyro=1\ngyro_sensitivity="+std::to_string(sensitivity)+"\n")==0);
        assert(qos.gyro&&qos.gyroSensitivity==sensitivity);
    }
    assert(load("gyro_invert_x=1\ngyro_invert_y=1\n")==0);
    assert(qos.gyroInvertX&&qos.gyroInvertY);
    std::remove(argv[1]);
    std::printf("PASS %u lifecycle states; touch cancellation and duplicate assignments; transactional shortcut/QoL parsing\n",checked);
}
