// Included in the single GPU/platform TU. Modal input runs on the game thread;
// framebuffer writes run on the established render owner, after pending work.
#pragma once
#include "settings_view_3ds.h"
extern "C" void n3dsSettingsClockPause(int);
extern "C" void n3dsAudioSettingsPause(int);
extern "C" void n3dsSettingsRebase(uint64_t);
extern "C" long long OSGetTime(void);
namespace citro_settings {
inline void menuPaint(void* ptr){
 C3Di_RenderQueueWaitDone();u16 w=0,h=0;auto* raw=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h);
 if(!raw||w!=240||h!=320||gfxGetScreenFormat(GFX_BOTTOM)!=GSP_RGB565_OES)return;
 auto* pair=static_cast<std::pair<Model*,bool>*>(ptr);draw(Canvas{reinterpret_cast<uint16_t*>(raw)},*pair->first,pair->second);
 if(std::getenv("N3DS_SETTINGS_TEST")){if(FILE* f=std::fopen(N3DS_ROOT "/settings.rgb565","wb")){std::fwrite(raw,1,size_t(w)*h*2,f);std::fclose(f);}}
 gfxFlushBuffers();gfxScreenSwapBuffers(GFX_BOTTOM,false);
}
inline void menuRestore(void*){
 C3Di_RenderQueueWaitDone();g_bottomUi.init();g_lastPresent=0;
 if(g_perf.bottomUi)g_bottomUi.update(0,0);gfxFlushBuffers();gfxScreenSwapBuffers(GFX_BOTTOM,false);
 if(!g_perf.bottomUi){g_bottomUi.init();gfxFlushBuffers();gfxScreenSwapBuffers(GFX_BOTTOM,false);}
}
inline void owner(void (*fn)(void*),void* p){if(n3dsRenderThreadActive())n3dsRenderThreadJob(fn,p);else fn(p);}
struct ModalGuard {
 uint64_t start;
 ModalGuard():start(svcGetSystemTick()){n3dsAudioSettingsPause(1);n3dsSettingsClockPause(1);n3dsGpuSync();}
 ~ModalGuard(){const auto elapsed=svcGetSystemTick()-start;n3dsSettingsClockPause(0);n3dsAudioSettingsPause(0);n3dsSettingsRebase(elapsed);owner(menuRestore,nullptr);}
};
inline void menuApplyPresentation(void* ptr){
 const auto& m=*static_cast<Model*>(ptr);
 g_perf.bottomUi=m.value[1]!=0;g_perf.uiMapBottom=m.value[1]==2;
 if(g_r)g_r->setUiRoute(g_perf.bottomUi&&g_perf.uiMapBottom,[](bool owns){
  g_routeOwnsBottom.store(owns,std::memory_order_release);n3dsUiSetGpuOwnsBottom(owns?1:0);
 });
 g_bottomUi.init();
 n3dsLog("[settings] bottom screen live mode %d\n",m.value[1]);
}
inline void runMenu(bool test){
 ModalGuard guard;
 static std::unique_ptr<Model> pending;
 if(!pending)pending=std::make_unique<Model>(g_perf);
 Model m=*pending;m.saved=false;bool error=false,done=false,save=false,dirty=true;
 auto back=[&]{if(m.value!=pending->value)save=true;else done=true;};
 const uint64_t start=svcGetSystemTick();const long long gameBefore=OSGetTime();n3dsLog("[settings] opened; game clock frozen\n");
 while(!done&&aptMainLoop()){
  if(dirty){std::pair<Model*,bool> job{&m,error};owner(menuPaint,&job);dirty=false;}
  hidScanInput();const u32 down=hidKeysDown();
  if(down&KEY_L){m.tab(-1);dirty=true;}if(down&KEY_R){m.tab(1);dirty=true;}
  if(down&KEY_UP){m.move(-1);dirty=true;}if(down&KEY_DOWN){m.move(1);dirty=true;}
  if(down&KEY_LEFT){m.cycle(-1);dirty=true;}if(down&(KEY_RIGHT|KEY_A)){m.cycle(1);dirty=true;}
  if(down&KEY_B)back();
  if(down&KEY_Y)save=true;
  if(error&&(down&KEY_X)){done=true;n3dsLog("[settings] discarded edits after save failure\n");}
  if(down&KEY_TOUCH){touchPosition t{};hidTouchRead(&t);
   if(t.py>=36&&t.py<59){m.page=t.px<95?0:t.px<203?1:2;m.selected=0;dirty=true;}
   else if(t.py>=65&&t.py<65+m.count()*23&&t.px>=12&&t.px<308){m.selected=(t.py-65)/23;m.cycle(1);dirty=true;}
   else if(t.py>=213&&t.py<233){if(t.px<136)back();else if(t.px>=141)save=true;}
  }
  if(test&&std::getenv("N3DS_SETTINGS_TEST")&&std::strcmp(std::getenv("N3DS_SETTINGS_TEST"),"save")==0&&svcGetSystemTick()-start>SYSCLOCK_ARM11/2){m.value[3]=1;save=true;}
  if(save){save=false;if(m.save(N3DS_ROOT "/performance.ini")){*pending=m;owner(menuApplyPresentation,&m);fpsEnabled.store(m.value[3]!=0,std::memory_order_relaxed);if(n3dsUiSetTopHudEnabled)n3dsUiSetTopHudEnabled(g_perf.widescreen?m.value[2]:0);m.saved=true;{std::pair<Model*,bool> job{&m,false};owner(menuPaint,&job);}svcSleepThread(750000000);done=true;n3dsLog("[settings] saved; bottom/HUD/counter applied; layout/worker changes apply next launch\n");}else {n3dsLog("[settings] save failed errno %d (%s); X returns without saving\n",errno,std::strerror(errno));error=true;dirty=true;}}
  if(test&&std::getenv("N3DS_SETTINGS_TEST")&&std::strcmp(std::getenv("N3DS_SETTINGS_TEST"),"map")==0&&svcGetSystemTick()-start>SYSCLOCK_ARM11/2){m.value[1]=2;back();}
  if(test&&svcGetSystemTick()-start>SYSCLOCK_ARM11){
   done=true;
  }
  svcSleepThread(16000000);
 }
 const long long gameAfter=OSGetTime();const uint64_t elapsed=svcGetSystemTick()-start;
 n3dsLog("[settings] closed after %llu ms; paused game-clock delta %lld ticks\n",(unsigned long long)(elapsed*1000/SYSCLOCK_ARM11),gameAfter-gameBefore);
}
}
