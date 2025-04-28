#include "../../kernel/include/kernel.h"

#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/eventflag.h>

static uint32_t bits;
static SceUID thid = 0;
static SceUID evtflag_user;
static SceUID pebble_mtx_uid_user;
static SceDisplayFrameBuf user_frame;
static uint32_t *fb_bases_user[2] = {NULL, NULL};
static SceUID user_buffer_uids[2] = {0, 0};

int pebble_thread_user(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    SceUID PID_user = sceKernelGetProcessId();
    pebble_mtx_uid_user = sceKernelCreateMutex("pebble_gui_mutex", 0, 0, NULL);
    user_buffer_uids[0] = sceKernelAllocMemBlock("user_buffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0x200000, NULL);
    if (user_buffer_uids[0] > 0)
        sceKernelGetMemBlockBase(user_buffer_uids[0], (void **)&fb_bases_user[0]);
    else
        sceClibPrintf("Failed for buffer0000 allocating!!!!!\n");

    user_buffer_uids[1] = sceKernelAllocMemBlock("user_buffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0x200000, NULL);
    if (user_buffer_uids[1] > 0)
        sceKernelGetMemBlockBase(user_buffer_uids[1], (void **)&fb_bases_user[1]);
    else
        sceClibPrintf("Failed for buffer1111 allocating!!!!!\n");

    user_frame.size = sizeof(SceDisplayFrameBuf);
    user_frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    user_frame.width = 960;
    user_frame.pitch = 960;
    user_frame.height = 544;
    evtflag_user = sceKernelCreateEventFlag("ongui", SCE_KERNEL_ATTR_THREAD_FIFO, 0, NULL);
    if (PID_user && pebble_mtx_uid_user > 0 && evtflag_user > 0 && user_buffer_uids[0] > 0 && user_buffer_uids[1] > 0)
        kernel_get_userinfo(PID_user, pebble_mtx_uid_user, fb_bases_user[0], evtflag_user);

    //sceClibPrintf("FB Context: %#X, %#X\n", fb_bases_user[0], fb_bases_user[1]);
    
    while (1)
    {
        sceKernelWaitEventFlag(evtflag_user, 3, (SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT), &bits, NULL);
        if (bits & 1)
            user_frame.base = fb_bases_user[0];
        else
            user_frame.base = fb_bases_user[1];
        if (sceKernelLockMutex(pebble_mtx_uid_user, 1, NULL) == 0)
        {
            sceDisplaySetFrameBuf(&user_frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
            sceKernelUnlockMutex(pebble_mtx_uid_user, 1);
        }
    }
    return 0;
}

void _start() __attribute__((weak, alias("module_start")));
void module_start(void) {
    thid = sceKernelCreateThread("pebble_user", pebble_thread_user, 0x40, 0x500, 0, 0, NULL);
    if (thid > 0)
        sceKernelStartThread(thid, 0, NULL);
}

void module_stop(void) {
    if (evtflag_user)
        sceKernelDeleteEventFlag(evtflag_user);
    if (thid)
        sceKernelDeleteThread(thid);
    if (pebble_mtx_uid_user)
        sceKernelDeleteMutex(pebble_mtx_uid_user);
    fb_bases_user[0] = NULL;
    fb_bases_user[1] = NULL;
    user_buffer_uids[0] = 0;
    user_buffer_uids[1] = 0;
}