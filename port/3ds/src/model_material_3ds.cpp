#include <cstdint>
extern "C" unsigned g_n3dsMaterialIdentity;
extern "C" void n3dsMaterialWriting(const void*,uint32_t);
extern "C" void __real_GDInitGDLObj(void*,void*,uint32_t);
extern "C" void __wrap_GDInitGDLObj(void* object,void* start,uint32_t bytes){
 if(g_n3dsMaterialIdentity)n3dsMaterialWriting(start,bytes);
 __real_GDInitGDLObj(object,start,bytes);
}
