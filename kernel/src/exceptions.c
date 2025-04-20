#include "kernel.h"

extern void handler_asm_pabt(void);
extern void handler_asm_dabt(void);
extern void handler_asm_undef(void);
SceThreadCpuRegisters all_registers;
SceArmCpuRegisters current_registers;

int exception_handler(void) {
    SceKernelThreadContextInfo info;
    if (ksceKernelGetThreadContextInfo(&info) < 0) 
        return EXCEPTION_NOT_HANDLED;
    if (g_target_process.pid != info.process_id) 
        return EXCEPTION_NOT_HANDLED;

    g_target_process.exception_thid = info.thread_id;
    if (ksceKernelGetThreadCpuRegisters(info.thread_id, &all_registers) < 0) 
        return EXCEPTION_NOT_HANDLED;
	
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

    ksceKernelChangeThreadSuspendStatus(info.thread_id, 0x1002);
    return EXCEPTION_NOT_HANDLED;
}

int register_handler(void) {
    if(ksceExcpmgrRegisterHandler(SCE_EXCP_PABT, 5, (void *)handler_asm_pabt) < 0)
        return -1;
    if(ksceExcpmgrRegisterHandler(SCE_EXCP_DABT, 5, (void *)handler_asm_dabt) < 0)
        return -1;
    if(ksceExcpmgrRegisterHandler(SCE_EXCP_UNDEF_INSTRUCTION, 5, (void *)handler_asm_undef) < 0)
        return -1;
    
    return 0;
}
