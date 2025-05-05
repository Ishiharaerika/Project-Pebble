#include "kernel.h"

SceThreadCpuRegisters all_registers;
SceArmCpuRegisters current_registers;

extern void asm_pabt(void);
extern void asm_dabt(void);
extern void asm_undef(void);

int handle_create(SceUID pid, SceProcEventInvokeParam2 *a2, int a3)
{
    (void)a2;
    (void)a3;

    g_target_process.pid = pid;
    guistate.gui_visible = false;
    guistate.ui_state = UI_WELCOME;
    guistate.edit_mode = EDIT_NONE;
    guistate.view_state = VIEW_STACK;
    guistate.mem_layout = MEM_LAYOUT_8BIT;
    load_hotkeys();
    kernel_debugger_on_create();
    
    return 0;
}

int handle_kill(SceUID pid, SceProcEventInvokeParam1 *a2, int a3)
{
    (void)a2;
    (void)a3;
    for (int i = 0; i < MAX_HW_BKPT; ++i) // Clear HW BKPT & Single step BKPT
    {
        if (guistate.breakpoints[i].pid == pid)
            kernel_clear_breakpoint(i);
    }
    kernel_debugger_init();

    return 0;
}

const SceProcEventHandler handler = {
    .size = 0x1C,
    .create = handle_create,
    .exit = NULL,
    .kill = handle_kill,
    .stop = NULL,
    .start = NULL,
    .switch_process = NULL // Does not support debugging when switching process.
};

int exception_handler(int exception_type, uint32_t dfar_value)
{
    SceKernelThreadContextInfo info;
    if (ksceKernelGetThreadContextInfo(&info) < 0 || info.process_id != g_target_process.pid)
        return SCE_EXCPMGR_EXCEPTION_HANDLED;
    ksceKernelPrintf("Stage 1\n");
    g_target_process.exception_thid = info.thread_id;

    if (ksceKernelGetThreadCpuRegisters(info.thread_id, &all_registers) < 0)
        return SCE_EXCPMGR_EXCEPTION_HANDLED;
    ksceKernelPrintf("Stage 2\n");
    memcpy(&current_registers, &all_registers.user, sizeof(SceArmCpuRegisters));

    bool is_thumb = (current_registers.cpsr & (1 << 5)) != 0;
    uint32_t bkpt_addr = current_registers.pc;

    // Adjust PC based on exception type
    if (exception_type == SCE_EXCP_PABT) // PABT
        bkpt_addr -= 4;
    else if (exception_type == SCE_EXCP_DABT) // DABT
        bkpt_addr -= 8;
    else if (exception_type == SCE_EXCP_UNDEF_INSTRUCTION) // UNDEF
        bkpt_addr -= is_thumb ? 2 : 4;
        
    ksceKernelPrintf("Stage 3\n");
    // Check if this exception is caused by one of the breakpoints
    bool handled = false;
    ActiveBKPTSlot *bp = guistate.breakpoints;
    for (int i = 0; i < MAX_SLOT; ++i, ++bp)
    {
        if (bp->type == SLOT_NONE || bp->pid != g_target_process.pid)
            continue;

        if (bp->address == bkpt_addr)
        {
            if ((bp->type == SW_BREAKPOINT_THUMB || bp->type == SW_BREAKPOINT_ARM) && 
                 exception_type == SCE_EXCP_UNDEF_INSTRUCTION)
            {
                uint32_t size = (bp->type == SW_BREAKPOINT_THUMB) ? 2 : 4;
                if (ksceKernelCopyToUserProcTextDomain(g_target_process.pid, (void *)bkpt_addr, 
                    &bp->p_instruction, size) >= 0) {
                    handled = true;
                    break;
                }
            }
            else if (bp->type == HW_BREAKPOINT && exception_type == SCE_EXCP_PABT)
            {
                handled = true;
                break;
            }
            else if ((bp->type == HW_WATCHPOINT_R || bp->type == HW_WATCHPOINT_W || bp->type == HW_WATCHPOINT_RW) && exception_type == SCE_EXCP_DABT)
            {
                uint32_t wp_size = 4;
                if (dfar_value >= bp->address && dfar_value < (bp->address + wp_size))
                {
                    handled = true;
                    break;
                }
            }
            else if (i == SINGLE_STEP_SLOT && bp->type == SINGLE_STEP_HW_BREAKPOINT && 
                     exception_type == SCE_EXCP_PABT)
            {
                kernel_clear_breakpoint(SINGLE_STEP_SLOT);
                handled = true;
                break;
            }
        }
    }

    if (handled)
    {
        ksceKernelPrintf("Stage 4\n");
        guistate.gui_visible = true;
        ksceKernelChangeThreadSuspendStatus(info.thread_id, 0x1002);
    }

    return SCE_EXCPMGR_EXCEPTION_HANDLED; // Always return handled to avoid crashes
}

int register_handler(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    if (ksceExcpmgrRegisterHandler(SCE_EXCP_DABT, 0, (void *)asm_dabt) < 0)
        return ksceKernelPrintf("Failed registering DABT handler.\n");

    //if (ksceExcpmgrRegisterHandler(SCE_EXCP_PABT, 3, (void *)asm_pabt) < 0)
    //    return ksceKernelPrintf("Failed registering PABT handler.\n");

    //if (ksceExcpmgrRegisterHandler(SCE_EXCP_UNDEF_INSTRUCTION, 3, (void *)asm_undef) < 0)
    //    return ksceKernelPrintf("Failed registering UNDEF handler.\n");
#pragma GCC diagnostic pop

    if (ksceKernelRegisterProcEventHandler("pebbleBKPT", &handler, 0) < 0)
        return ksceKernelPrintf("Failed registering ProcEventHandler.\n");

    ksceKernelPrintf("Successfully registered all exception handlers.\n");
    return 0;
}