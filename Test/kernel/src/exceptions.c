#include "kernel.h"

SceThreadCpuRegisters all_registers;
SceArmCpuRegisters current_registers;
SceUID pebble_mtx_uid = 0;
bool ksceAppMgrIsExclusiveProcessRunning();
bool isExclusive = false;

extern void (*handler_asm_pabt)(void);
extern void (*handler_asm_dabt)(void);
extern void (*handler_asm_undef)(void);

int handle_create(SceUID pid, SceProcEventInvokeParam2 *a2, int a3)
{
    (void)a2;
    (void)a3;

    if (!pebble_mtx_uid)
    {
        pebble_mtx_uid = ksceKernelCreateMutex("pebble_gui_mutex", 0, 0, NULL);
        ksceKernelPrintf("!!!Mutex Created!!!\n");
    }

    g_target_process.pid = pid;
    guistate.ui_state = UI_WELCOME;
    guistate.edit_mode = EDIT_NONE;
    guistate.view_state = VIEW_STACK;
    guistate.mem_layout = MEM_LAYOUT_8BIT;
    load_hotkeys();
    isExclusive = ksceAppMgrIsExclusiveProcessRunning();
    kernel_debugger_on_create();

    return 0;
}

int handle_kill(SceUID pid, SceProcEventInvokeParam1 *a2, int a3)
{
    (void)pid;
    (void)a2;
    (void)a3;
    kernel_debugger_init();
    isExclusive = false;

    if (pebble_mtx_uid)
    {
        ksceKernelDeleteMutex(pebble_mtx_uid);
        ksceKernelPrintf("!!!Mutex Deleted!!!\n");
        pebble_mtx_uid = 0;
    }

    return 0;
}

const SceProcEventHandler handler = {
    .size = 0x1C,
    .create = handle_create,
    .exit = NULL,
    .kill = handle_kill,
    .stop = NULL,
    .start = NULL,
    .switch_process = NULL // Does not support debug while switching process.
};

int exception_handler(int exception_type)
{
    SceKernelThreadContextInfo info;
    if (ksceKernelGetThreadContextInfo(&info) < 0 || info.process_id != g_target_process.pid)
        return EXCEPTION_NOT_HANDLED;

    uint32_t spsr;
    asm volatile("mrs %0, spsr" : "=r" (spsr));
    if ((spsr & 0x1F) != 0x10) // Check if mode bits indicate user mode (0x10)
        return EXCEPTION_NOT_HANDLED;
    
    g_target_process.exception_thid = info.thread_id;
    
    if (ksceKernelGetThreadCpuRegisters(info.thread_id, &all_registers) < 0)
        return EXCEPTION_NOT_HANDLED;
    
    memcpy(&current_registers, &all_registers.user, sizeof(SceArmCpuRegisters));
    
    // Calculate the actual breakpoint address based on instruction mode
    bool is_thumb = (current_registers.cpsr & (1 << 5)) != 0;
    uint32_t bkpt_addr = current_registers.pc;
    
    // Adjust PC based on exception type
    if (exception_type == 0) // DABT
        bkpt_addr -= (is_thumb ? 8 : 4);
    else if (exception_type == 1) // PABT
        bkpt_addr -= (is_thumb ? 4 : 4);
    else if (exception_type == 2) // UNDEF
        bkpt_addr -= (is_thumb ? 2 : 4);

    // Restore original instruction if it's a software breakpoint
    for (int i = 0; i < MAX_SLOT; ++i)
    {
        if ((guistate.breakpoints[i].type == SW_BREAKPOINT_THUMB || guistate.breakpoints[i].type == SW_BREAKPOINT_ARM) &&
            guistate.breakpoints[i].address == bkpt_addr)
        {
            uint32_t size = (guistate.breakpoints[i].type == SW_BREAKPOINT_THUMB) ? 2 : 4;
            ksceKernelCopyToUserProcTextDomain(g_target_process.pid, (void *)bkpt_addr, &guistate.breakpoints[i].p_instruction, size);
            break;
        }
    }
    
    // Clear single step breakpoint if hit
    if (guistate.breakpoints[SINGLE_STEP_SLOT].type == SINGLE_STEP_HW_BREAKPOINT &&
        guistate.breakpoints[SINGLE_STEP_SLOT].address == bkpt_addr)
        kernel_clear_breakpoint(SINGLE_STEP_SLOT);

    // Show debugger UI and suspend the thread
    guistate.gui_visible = true;
    ksceKernelChangeThreadSuspendStatus(info.thread_id, 0x1002);
    
    ksceKernelPrintf("Exception handled: type=%d, PC=0x%08X, thread=%d\n", exception_type, bkpt_addr, info.thread_id);
                    
    return EXCEPTION_NOT_HANDLED;
}

int register_handler(void)
{
    //if (ksceExcpmgrRegisterHandler(SCE_EXCP_DABT, 10, (void *)&handler_asm_dabt) < 0)
    //    return ksceKernelPrintf("Failed registering DABT handler.\n");

    //if (ksceExcpmgrRegisterHandler(SCE_EXCP_PABT, 10, (void *)&handler_asm_pabt) < 0)
    //    return ksceKernelPrintf("Failed registering PABT handler.\n");

    //if (ksceExcpmgrRegisterHandler(SCE_EXCP_UNDEF_INSTRUCTION, 10, (void *)&handler_asm_undef) < 0)
    //    return ksceKernelPrintf("Failed registering UNDEF handler.\n");

    if (ksceKernelRegisterProcEventHandler("pebbleBKPT", &handler, 0) < 0)
        return ksceKernelPrintf("Failed registering ProcEventHandler.\n");

    ksceKernelPrintf("Successfully registered all exception handlers.\n");
    return 0;
}