#include "kernel.h"

SceThreadCpuRegisters all_registers;
SceArmCpuRegisters current_registers;
SceUID g_breakpoint_triggered = -1;

extern void handler_asm_pabt(void);
extern void handler_asm_dabt(void);
extern void handler_asm_undef(void);

//int proc_create(SceUID pid, SceProcEventInvokeParam2 *a2, int a3){
//  char titleid[0x20];
//  ksceKernelSysrootGetProcessTitleId(pid, titleid, sizeof(titleid));
//  ksceKernelPrintf("titleid=%s\n", titleid);
//     ksceKernelPrintf("Proc Created!!!\n");
//
//  return 0;
//}
//
//const SceProcEventHandler handler = {
//  .size           = sizeof(SceProcEventHandler),
//  .create         = proc_create,
//  .exit           = NULL,
//  .kill           = NULL,
//  .stop           = NULL,
//  .start          = NULL,
//  .switch_process = NULL
//};

int exception_handler(void) {
    SceKernelThreadContextInfo info;
    if (ksceKernelGetThreadContextInfo(&info) < 0) {
        return EXCEPTION_NOT_HANDLED;
    }
    if (g_target_process.pid != info.process_id) {
        return EXCEPTION_NOT_HANDLED;
    }

    g_target_process.exception_thid = info.thread_id;
    if (ksceKernelGetThreadCpuRegisters(info.thread_id, &all_registers) < 0) {
        return EXCEPTION_NOT_HANDLED;
    }

    current_registers = ((all_registers.user.cpsr & 0x1F) == 0x10) ? all_registers.user : all_registers.kernel;
    uint32_t pc_addr = current_registers.pc;

    // Restore original instruction if it's a software breakpoint
    for (int i = 0; i < MAX_SLOT; ++i) {
        if ((g_active_slot[i].type == SW_BREAKPOINT_THUMB || g_active_slot[i].type == SW_BREAKPOINT_ARM) &&
            g_active_slot[i].address == pc_addr) {
            uint32_t size = (g_active_slot[i].type == SW_BREAKPOINT_THUMB) ? 2 : 4;
            ksceKernelCopyToUserProcTextDomain(g_target_process.pid, (void *)pc_addr, &g_active_slot[i].p_instruction, size);
            break;
        }
    }

    // Clear single step breakpoint if hit
    if (g_active_slot[SINGLE_STEP_SLOT].type == SINGLE_STEP_HW_BREAKPOINT &&
        g_active_slot[SINGLE_STEP_SLOT].address == pc_addr) {
        kernel_clear_breakpoint(SINGLE_STEP_SLOT);
    }

    ksceKernelSetEventFlag(g_breakpoint_triggered, 0x1);
    ksceKernelChangeThreadSuspendStatus(info.thread_id, 0x1002);
    return EXCEPTION_NOT_HANDLED;
}

int register_handler(void) {
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    if (ksceExcpmgrRegisterHandler(SCE_EXCP_PABT, 5, (void *)&handler_asm_pabt) < 0) {
        return ksceKernelPrintf("Failed registering handler1.");
    }
    if (ksceExcpmgrRegisterHandler(SCE_EXCP_DABT, 5, (void *)&handler_asm_dabt) < 0) {
        return ksceKernelPrintf("Failed registering handler2.");
    }
    if (ksceExcpmgrRegisterHandler(SCE_EXCP_UNDEF_INSTRUCTION, 5, (void *)&handler_asm_undef) < 0) {
        return ksceKernelPrintf("Failed registering handler3.");
    }
    #pragma GCC diagnostic pop
    g_breakpoint_triggered = ksceKernelCreateEventFlag("BKPTTreggered", SCE_KERNEL_ATTR_THREAD_FIFO, 0, NULL);

    //ksceKernelRegisterProcEventHandler("SceArmHwbkpt", &handler, 0);
    return ksceKernelPrintf("Successfully registered handlers.");
}