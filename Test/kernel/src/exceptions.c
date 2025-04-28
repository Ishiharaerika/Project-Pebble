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
    (void)pid;
    (void)a2;
    (void)a3;
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
    .switch_process = NULL // Does not support debug while switching process.
};

int exception_handler(int exception_type)
{
    SceKernelThreadContextInfo info;
    //uint32_t spsr;
    //asm volatile("mrs %0, spsr" : "=r"(spsr));
    //if ((spsr & 0x1F) != 0x10)
    //    return EXCEPTION_NOT_HANDLED;
    if (ksceKernelGetThreadContextInfo(&info) < 0 || info.process_id != g_target_process.pid)
        return EXCEPTION_NOT_HANDLED;
    g_target_process.exception_thid = info.thread_id;
    if (ksceKernelGetThreadCpuRegisters(info.thread_id, &all_registers) < 0)
        return EXCEPTION_NOT_HANDLED;
    memcpy(&current_registers, &all_registers.user, sizeof(SceArmCpuRegisters));
    bool is_thumb = (current_registers.cpsr & (1 << 5)) != 0;
    uint32_t bkpt_addr = current_registers.pc;
    uint32_t pc_offset = 0;
    if (exception_type == 0)
        pc_offset = is_thumb ? 8 : 4;
    else if (exception_type == 1)
        pc_offset = 4;
    else if (exception_type == 2)
        pc_offset = is_thumb ? 2 : 4;
    bkpt_addr -= pc_offset;
    ActiveBKPTSlot *bp = guistate.breakpoints;
    for (int i = 0; i < MAX_SLOT; ++i, ++bp)
    {
        if (bp->address == bkpt_addr)
        {
            if (bp->type == SW_BREAKPOINT_THUMB || bp->type == SW_BREAKPOINT_ARM)
            {
                uint32_t size = (bp->type == SW_BREAKPOINT_THUMB) ? 2 : 4;
                ksceKernelCopyToUserProcTextDomain(g_target_process.pid, (void *)bkpt_addr, &bp->p_instruction, size);
                break;
            }
            else if (i == SINGLE_STEP_SLOT && bp->type == SINGLE_STEP_HW_BREAKPOINT)
                kernel_clear_breakpoint(SINGLE_STEP_SLOT);
        }
    }
    guistate.gui_visible = true;
    ksceKernelChangeThreadSuspendStatus(info.thread_id, 0x1002);
    return EXCEPTION_NOT_HANDLED;
}

int register_handler(void)
{
    //if (ksceExcpmgrRegisterHandler(SCE_EXCP_DABT, 10, (void *)asm_dabt) < 0)
    //    return ksceKernelPrintf("Failed registering DABT handler.\n");

    //if (ksceExcpmgrRegisterHandler(SCE_EXCP_PABT, 10, (void *)asm_pabt) < 0)
    //    return ksceKernelPrintf("Failed registering PABT handler.\n");

    //if (ksceExcpmgrRegisterHandler(SCE_EXCP_UNDEF_INSTRUCTION, 10, (void *)asm_undef) < 0)
    //    return ksceKernelPrintf("Failed registering UNDEF handler.\n");

    if (ksceKernelRegisterProcEventHandler("pebbleBKPT", &handler, 0) < 0)
        return ksceKernelPrintf("Failed registering ProcEventHandler.\n");

    ksceKernelPrintf("Successfully registered all exception handlers.\n");
    return 0;
}