#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int g_n3dsTypedFrame;
extern int g_n3dsTypedPrimitive;
extern uint32_t g_n3dsTypedConsumedSize;
void n3dsTypedRegisterFifo(void* fifo);
void n3dsTypedUnregisterFifo(void* fifo);
void n3dsTypedBeginFrame(void);
void n3dsTypedEndFrame(void);
void n3dsTypedApplyBp(uint32_t value);
void n3dsTypedApplyCp(uint8_t address, uint32_t value);
void n3dsTypedApplyXf(uint32_t address, const uint8_t* bigEndianWords, uint32_t count);
#ifdef __cplusplus
}
#endif
