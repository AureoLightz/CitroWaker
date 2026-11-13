// UI-owned C bridge. No renderer types; input is sampled on the game thread.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum N3dsUiShortcut {
 N3DS_UI_NONE=0, N3DS_UI_ITEM_X=1u<<0, N3DS_UI_ITEM_Y=1u<<1,
 N3DS_UI_ITEM_Z=1u<<2, N3DS_UI_MENU=1u<<3, N3DS_UI_MAP_SIZE=1u<<4
};
// Default-off top-HUD presentation switch (performance.ini top_hud=0/1).
void n3dsUiSetTopHudEnabled(int on);
int n3dsUiTopHudEnabled(void);
// Game producer publishes an immutable lifecycle word (ui_lifecycle_3ds.h).
// Snapshot getter ONLY on producer; renderer must record the returned value.
void n3dsUiPublishPresentation(uint8_t alpha,uint32_t drawn,uint32_t usable,uint32_t blocked);
uint32_t n3dsUiGetPresentation(void);
// Receiver publishes ONLY the actions actually drawn and usable. Initially 0.
// Clear on title, cutscene/no-HUD, pause, suspension and backend disable.
void n3dsUiSetTouchEnabled(uint32_t actions);
// Game-owned source bounds; receiver may sample only on the producer inside Begin.
int n3dsUiMapGetSourceRect(int* left,int* top,int* width,int* height);
// Receiver brackets ownership of the bottom framebuffer; no CPU painting while on.
void n3dsUiSetGpuOwnsBottom(int owns);
uint32_t n3dsUiTouchAction(uint32_t physicalHeld, int inputBlocked);
uint32_t n3dsUiTouchHighlight(void);
// Startup-only optional shortcuts.ini;0 success,-1 absent,-2 unsafe layout,
// positive invalid line. A rejected file leaves all defaults intact.
int n3dsUiLoadShortcuts(void);
// Action-only configuration; immutable fixed slot geometry; snapshot on the producer, not core2.
int n3dsUiGetShortcut(unsigned slot,int rect[4],uint32_t* action);
// Distinguishes duplicate assignments: only the actual touched slot is pressed.
int n3dsUiGetTouchSlot(void);
// Real game item IDs, 0xFF = empty. Snapshot this on the producer into a UI
// event if drawing frame-exact icons; do not consult live game memory on core2.
void n3dsUiPublishItems(uint8_t x, uint8_t y, uint8_t z);
// Inventory-slot assignments distinguish re-equipping from bottle/arrow modes.
void n3dsUiPublishBindings(uint8_t x, uint8_t y, uint8_t z);
uint32_t n3dsUiGetItems(void);
#ifdef __cplusplus
}
#endif
