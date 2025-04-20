#include <psp2/vshbridge.h>
#include <psp2/kernel/clib.h>

#include <taihen.h>

 void _start() __attribute__ ((weak, alias("module_start")));
 int module_start(void){
     int search_unk[2];
     int res = _vshKernelSearchModuleByName("pebble", search_unk);
     if (res > 0 ) {
        sceClibPrintf("Successfully loaded module (modid: 0x%X)\n", res);
        return SCE_KERNEL_START_SUCCESS;
     } else {
        sceClibPrintf("Failed loading module (modid: 0x%X)\n", res);
     }

     return SCE_KERNEL_START_FAILED;
 }