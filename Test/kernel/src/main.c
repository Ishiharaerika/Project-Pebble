#include "kernel.h"
#include "reVita.h"

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);
// int (*ksceKernelSetTHBP)(SceUID thid, SceUInt32 a2, void *BVR, SceUInt32 BCR);
int (*ksceKernelSetPHWP)(SceUID pid, SceUInt32 a2, void *WVR, SceUInt32 WCR);
int (*ksceKernelSetPHBP)(SceUID pid, SceUInt32 a2, void *BVR, SceUInt32 BCR);

TargetProcess g_target_process;
ActiveBKPTSlot g_active_slot[MAX_SLOT];
SceUID g_heap_uid = -1;

static int find_empty_slot(int start, int end)
{
    for (int i = start; i < end; ++i)
    {
        if (g_active_slot[i].type == SLOT_NONE)
            return i;
    }

    return -1;
}

static void clear_slot(ActiveBKPTSlot *slot)
{
    if (slot)
    {
        slot->type = SLOT_NONE;
        slot->pid = 0;
        slot->address = 0;
        slot->index = 0xFF;
        slot->p_instruction = 0x0;
    }
}

int kernel_debugger_attach(SceUID pid)
{
    g_target_process.pid = pid;
    g_target_process.main_module_id = ksceKernelGetModuleIdByPid(pid);
    g_target_process.main_thread_id = ksceKernelGetProcessMainThread(pid);
    g_target_process.exception_thid = -1;

    return 0;
}

void kernel_debugger_init(void)
{
    g_target_process.pid = -1;
    g_target_process.main_module_id = -1;
    g_target_process.main_thread_id = -1;
    g_target_process.exception_thid = -1;
}

int kernel_set_hardware_breakpoint(uint32_t address)
{
    int index = find_empty_slot(0, SINGLE_STEP_SLOT);
    if (index == -1)
        return -1;

    uint32_t BCR = (1 << 0) | (0x3 << 1) | (0xF << 5) | (0x1 << 14) | (0x0 << 20);

    if (ksceKernelSetPHBP(g_target_process.pid, index, (void *)address, BCR) >= 0)
    {
        ActiveBKPTSlot *slot = &g_active_slot[index];
        slot->pid = g_target_process.pid;
        slot->address = address;
        slot->index = index;
        slot->type = HW_BREAKPOINT;
        ksceKernelPrintf("Hardware breakpoint set for PID 0x%08X in slot %d\n", g_target_process.pid, index);
        return 0;
    }
    ksceKernelPrintf("Failed to set hardware breakpoint for PID 0x%08X\n", g_target_process.pid);

    return -1;
}

int kernel_set_watchpoint(uint32_t address, WatchPointBreakType type)
{
    int index = find_empty_slot(0, SINGLE_STEP_SLOT);
    if (index == -1)
        return -1;

    uint32_t LSC;
    switch (type)
    {
    case BREAK_READ:
        LSC = 0x1;
        break;
    case BREAK_WRITE:
        LSC = 0x2;
        break;
    case BREAK_READ_WRITE:
        LSC = 0x3;
        break;
    default:
        return -6;
    }

    uint32_t WCR = (1 << 0) | (0x3 << 1) | (LSC << 3) | (0xF << 5) | (0x1 << 14);

    if (ksceKernelSetPHWP(g_target_process.pid, index, (void *)address, WCR) >= 0)
    {
        ActiveBKPTSlot *slot = &g_active_slot[index];
        slot->pid = g_target_process.pid;
        slot->address = address;
        slot->index = index;
        slot->type = (type == BREAK_READ)    ? HW_WATCHPOINT_R
                     : (type == BREAK_WRITE) ? HW_WATCHPOINT_W
                                             : HW_WATCHPOINT_RW;
        ksceKernelPrintf("Watchpoint set for PID 0x%08X in slot %d\n", g_target_process.pid, index);
        return 0;
    }
    ksceKernelPrintf("Failed to set watchpoint for PID 0x%08X\n", g_target_process.pid);

    return -1;
}

int kernel_set_software_breakpoint(uint32_t address, SlotType type)
{
    if ((type != SW_BREAKPOINT_THUMB && type != SW_BREAKPOINT_ARM))
        return -1;

    int index = find_empty_slot(MAX_HW_BKPT, MAX_SLOT);
    if (index == -1)
        return -1;

    uint32_t bkpt_instruction = (type == SW_BREAKPOINT_THUMB) ? SW_THUMB : SW_ARM;
    uint8_t instruction_size = (type == SW_BREAKPOINT_THUMB) ? 2 : 4;
    uint32_t original_instruction = 0;

    int read_ret;
    read_ret =
        ksceKernelCopyFromUserProc(g_target_process.pid, &original_instruction, (void *)address, instruction_size);

    if (read_ret < 0)
        return read_ret;

    int write_ret;
    write_ret =
        ksceKernelCopyToUserProcTextDomain(g_target_process.pid, (void *)address, &bkpt_instruction, instruction_size);

    if (write_ret < 0)
    {
        ksceKernelCopyToUserProcTextDomain(g_target_process.pid, (void *)address, &original_instruction, instruction_size);
        return write_ret;
    }

    ActiveBKPTSlot *slot = &g_active_slot[index];
    slot->pid = g_target_process.pid;
    slot->address = address;
    slot->index = index;
    slot->p_instruction = original_instruction;
    slot->type = type;
    ksceKernelPrintf("Software breakpoint set for PID 0x%08X in slot %d\n", g_target_process.pid, index);

    return write_ret;
}

int kernel_clear_breakpoint(int index)
{
    if (index < 0 || index >= MAX_SLOT)
        return -1;

    ActiveBKPTSlot *slot = &g_active_slot[index];
    if (slot->type == SLOT_NONE)
        return -1;

    SceUID pid = slot->pid;
    int ret = -1;

    if (slot->type == HW_BREAKPOINT || slot->type == SINGLE_STEP_HW_BREAKPOINT)
        ret = ksceKernelSetPHBP(pid, slot->index, 0, 0);
    else if (slot->type >= HW_WATCHPOINT_R && slot->type <= HW_WATCHPOINT_RW)
        ret = ksceKernelSetPHWP(pid, slot->index, 0, 0);
    else if (slot->type == SW_BREAKPOINT_THUMB || slot->type == SW_BREAKPOINT_ARM)
    {
        uint8_t size = (slot->type == SW_BREAKPOINT_THUMB) ? 2 : 4;
        ret = ksceKernelCopyToUserProcTextDomain(pid, (void *)slot->address, &slot->p_instruction, size);
    }
    if (ret == 0)
        clear_slot(slot);

    return ret;
}

int kernel_list_breakpoints(ActiveBKPTSlot *dst)
{
    if (dst == NULL)
        return -2;
    memcpy(dst, g_active_slot, sizeof(g_active_slot));

    return 0;
}

int kernel_get_registers(SceArmCpuRegisters *dst)
{
    if (dst == NULL)
        return -2;
    memcpy(dst, &current_registers, sizeof(SceArmCpuRegisters));

    return 0;
}

int kernel_get_callstack(uint32_t *dst, int depth)
{
    if (dst == NULL || depth <= 0)
        return -2;
    if (g_target_process.pid <= 0 || g_target_process.exception_thid == -1)
        return -1;

    ThreadCpuRegisters availability_check;
    if (ksceKernelGetThreadCpuRegisters(g_target_process.exception_thid, &availability_check) < 0)
    {
        ksceKernelPrintf("Let a breakpoint be triggered first to use get_callstack.");
        return -1;
    }

    uint32_t call_stack_buffer[MAX_CALL_STACK_DEPTH];
    int current_depth = 0;
    int max_depth = (depth < MAX_CALL_STACK_DEPTH) ? depth : MAX_CALL_STACK_DEPTH;

    call_stack_buffer[current_depth++] = current_registers.pc;
    uint32_t current_fp = current_registers.r11;

    while (current_depth < max_depth)
    {
        if (current_fp == 0 || (current_fp & 3) != 0)
            break;

        uint32_t saved_lr, saved_fp;

        int read_lr_ret;
        read_lr_ret =
            ksceKernelCopyFromUserProc(g_target_process.pid, &saved_lr, (void *)(current_fp - 4), sizeof(saved_lr));

        int read_fp_ret;
        read_fp_ret = ksceKernelCopyFromUserProc(g_target_process.pid, &saved_fp, (void *)current_fp, sizeof(saved_fp));

        if (read_lr_ret < 0 || read_fp_ret < 0)
            break;
        if (saved_lr == 0 || saved_fp == current_fp)
            break;

        call_stack_buffer[current_depth++] = saved_lr;
        current_fp = saved_fp;
    }
    int copy_ret;
    copy_ret = (int)memcpy(dst, &call_stack_buffer, current_depth * sizeof(uint32_t));

    return copy_ret < 0 ? copy_ret : current_depth;
}

int kernel_get_modulelist(SceUID *user_modids, SceSize *user_num)
{
    if (g_target_process.pid <= 0 || user_modids == NULL || user_num == NULL)
    {
        ksceKernelPrintf("Attach to a process first to use GET_MODULELIST.");
        return -1;
    }

    return ksceKernelGetModuleList(g_target_process.pid, 0x7FFFFFFF, 1, (void *)user_modids, (void *)user_num) < 0 ? -1 : 0;
}

int kernel_get_moduleinfo(SceKernelModuleInfo *module_info)
{
    if (g_target_process.pid <= 0 || module_info == NULL)
    {
        ksceKernelPrintf("Attach to a process first to use GET_MODULEINFO.");
        return -1;
    }

    return ksceKernelGetModuleInfo(g_target_process.pid, g_target_process.main_module_id, (void *)module_info);
}

int kernel_get_memblockinfo(const void *address, unsigned info)
{
    SceUID ret = ksceKernelFindProcMemBlockByAddr(g_target_process.pid, address, 0);
    if (ret > 0)
        return ksceKernelGetMemBlockType(ret, &info);
    return -1;
}

int kernel_suspend_process(void)
{
    if (g_target_process.pid <= 0)
    {
        ksceKernelPrintf("Attach to a process first to use SUSPEND.");
        return -1;
    }

    return ksceKernelSuspendProcess(g_target_process.pid, 0x1C);
}

int kernel_resume_process(void)
{
    ThreadCpuRegisters availability_check;
    if (ksceKernelGetThreadCpuRegisters(g_target_process.exception_thid, &availability_check) < 0)
    {
        ksceKernelPrintf("Let a breakpoint be triggered first to use RESUME.");
        return -1;
    }
    ksceKernelChangeThreadSuspendStatus(g_target_process.exception_thid, 2);

    return ksceKernelResumeProcess(g_target_process.pid);
}

int kernel_single_step(void)
{
    if (g_active_slot[SINGLE_STEP_SLOT].type != SLOT_NONE)
        return -1;

    if (ksceKernelGetThreadCpuRegisters(g_target_process.exception_thid, &all_registers) < 0)
    {
        ksceKernelPrintf("Let a breakpoint be triggered first to use STEP.");
        return -1;
    }

    uint32_t current_pc = current_registers.pc;
    uint32_t next_pc;
    uint32_t instruction = 0;

    // Read current instruction
    int read_ret;
    read_ret = ksceKernelCopyFromUserProc(g_target_process.pid, &instruction, (void *)current_pc, 4);

    if (read_ret < 0)
        return read_ret;

    // Determine next PC based on current instruction
    if ((current_registers.cpsr & (1 << 5)) != 0)
    {
        // Thumb mode
        uint16_t thumb_instr = instruction & 0xFFFF;

        if ((thumb_instr & 0xF800) == 0xE000)
        {
            // Unconditional branch (B)
            int32_t offset = ((thumb_instr & 0x7FF) << 1);
            if (offset & 0x800)
                offset |= 0xFFFFF000; // Sign extend
            next_pc = current_pc + 2 + offset;
        }
        else if ((thumb_instr & 0xF000) == 0xD000)
        {
            // Conditional branch (B<cond>)
            int32_t offset = ((thumb_instr & 0xFF) << 1);
            if (offset & 0x100)
                offset |= 0xFFFFFE00; // Sign extend

            uint32_t cond = (thumb_instr >> 8) & 0xF;
            int condition_met = 0;
            uint32_t cpsr = current_registers.cpsr;

            switch (cond)
            {
            case 0:
                condition_met = (cpsr & (1 << 30)) != 0;
                break; // EQ: Z set
            case 1:
                condition_met = (cpsr & (1 << 30)) == 0;
                break; // NE: Z clear
            case 2:
                condition_met = (cpsr & (1 << 29)) != 0;
                break; // CS/HS: C set
            case 3:
                condition_met = (cpsr & (1 << 29)) == 0;
                break; // CC/LO: C clear
            case 4:
                condition_met = (cpsr & (1 << 31)) != 0;
                break; // MI: N set
            case 5:
                condition_met = (cpsr & (1 << 31)) == 0;
                break; // PL: N clear
            case 6:
                condition_met = (cpsr & (1 << 28)) != 0;
                break; // VS: V set
            case 7:
                condition_met = (cpsr & (1 << 28)) == 0;
                break; // VC: V clear
            case 8:
                condition_met = ((cpsr & (1 << 29)) != 0) && ((cpsr & (1 << 30)) == 0);
                break; // HI: C set and Z clear
            case 9:
                condition_met = ((cpsr & (1 << 29)) == 0) || ((cpsr & (1 << 30)) != 0);
                break; // LS: C clear or Z set
            case 10:
                condition_met = ((cpsr & (1 << 31)) >> 31) == ((cpsr & (1 << 28)) >> 28);
                break; // GE: N == V
            case 11:
                condition_met = ((cpsr & (1 << 31)) >> 31) != ((cpsr & (1 << 28)) >> 28);
                break; // LT: N != V
            case 12:
                condition_met = ((cpsr & (1 << 30)) == 0) && (((cpsr & (1 << 31)) >> 31) == ((cpsr & (1 << 28)) >> 28));
                break; // GT: Z clear AND (N == V)
            case 13:
                condition_met = ((cpsr & (1 << 30)) != 0) || (((cpsr & (1 << 31)) >> 31) != ((cpsr & (1 << 28)) >> 28));
                break; // LE: Z set OR (N != V)
            case 14:
                condition_met = 1;
                break; // AL: Always
            case 15:
                condition_met = 0;
                break;
            }

            next_pc = condition_met ? (current_pc + 2 + offset) : (current_pc + 2);
        }
        else if ((thumb_instr & 0xF800) == 0xF000 && (instruction & 0xF8000000) == 0xD0000000)
        {
            // Thumb-2 branch (32-bit instruction)
            uint32_t S = (thumb_instr >> 10) & 1;
            uint32_t imm10 = thumb_instr & 0x3FF;
            uint32_t J1 = (instruction >> 29) & 1;
            uint32_t J2 = (instruction >> 27) & 1;
            uint32_t imm11 = (instruction >> 16) & 0x7FF;

            int32_t offset = (S << 24) | (J1 << 23) | (J2 << 22) | (imm10 << 12) | (imm11 << 1);
            if (S)
                offset |= 0xFE000000; // Sign extend based on S bit

            next_pc = current_pc + 4 + offset; // PC is instruction addr + 4 for Thumb-2
        }
        else if ((thumb_instr & 0xFF00) == 0x4700)
            // BX Rm
            next_pc = current_pc + 2;
        else
            // Determine if it's a 16-bit or 32-bit Thumb instruction
            next_pc = current_pc + (((thumb_instr & 0xF800) >= 0xE800) ? 4 : 2);
    }
    else
    {
        // ARM mode
        if ((instruction & 0x0F000000) == 0x0A000000)
        {
            // Branch (B)
            int32_t offset = (instruction & 0x00FFFFFF) << 2;
            if (offset & 0x02000000)
                offset |= 0xFC000000; // Sign extend
            next_pc = current_pc + 8 + offset;
        }
        else if ((instruction & 0x0F000000) == 0x0B000000)
        {
            // Branch with Link (BL)
            int32_t offset = (instruction & 0x00FFFFFF) << 2;
            if (offset & 0x02000000)
                offset |= 0xFC000000; // Sign extend
            next_pc = current_pc + 8 + offset;
        }
        else
            // Not a branch or BX Rm
            next_pc = current_pc + 4;
    }

    // Set hardware breakpoint at the next instruction
    uint32_t BCR = (1 << 0) | (0x3 << 1) | (0xF << 5) | (0x1 << 14) | (0x0 << 20);
    int ret = -1;
    ret = ksceKernelSetPHBP(g_target_process.pid, SINGLE_STEP_SLOT, (void *)next_pc, BCR);

    if (ret >= 0)
    {
        ActiveBKPTSlot *slot = &g_active_slot[SINGLE_STEP_SLOT];
        slot->pid = g_target_process.pid;
        slot->address = next_pc;
        slot->index = SINGLE_STEP_SLOT;
        slot->type = SINGLE_STEP_HW_BREAKPOINT;

        ksceKernelChangeThreadSuspendStatus(g_target_process.exception_thid, 2);
        ksceKernelResumeProcess(g_target_process.pid);
    }

    return ret;
}

int kernel_read_memory(const void *src_addr, void *user_dst, SceSize size)
{
    if (src_addr == NULL || user_dst == NULL || size == 0)
        return -2;

    return ksceKernelCopyFromUserProc(g_target_process.pid, user_dst, src_addr, size);
}

int kernel_write_memory(void *dst, const void *user_modification, SceSize memwrite_len)
{
    if (!dst || !memwrite_len)
        return -2;

    void *buf = ksceKernelAllocHeapMemory(g_heap_uid, memwrite_len);
    if (!buf)
        return -2;

    if (ksceKernelCopyFromUserProc(g_target_process.pid, buf, user_modification, memwrite_len) >= 0)
    {
        uintptr_t info = 0;
        kernel_get_memblockinfo(dst, info);
        if (info & SCE_KERNEL_MEMBLOCK_TYPE_USER_RX)
            ksceKernelCopyToUserProcTextDomain(g_target_process.pid, dst, buf, memwrite_len);
        ksceKernelCopyToUserProc(g_target_process.pid, dst, buf, memwrite_len);
    }
    ksceKernelFreeHeapMemory(g_heap_uid, buf);

    return 0;
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(void)
{
    if ( // module_get_export_func(0x10005, "SceKernelThreadMgr", THREADMGR_NID, SETTHBP_NID, (uintptr_t*)&ksceKernelSetTHBP) < 0 ||
        module_get_export_func(0x10005, "SceProcessmgr", PROCESSMGR_NID, SETPHBP_NID, (uintptr_t *)&ksceKernelSetPHBP) < 0 ||
        module_get_export_func(0x10005, "SceProcessmgr", PROCESSMGR_NID, SETPHWP_NID, (uintptr_t *)&ksceKernelSetPHWP) < 0)
            return SCE_KERNEL_START_FAILED;

    for (int i = 0; i < MAX_SLOT; ++i)
        clear_slot(&g_active_slot[i]);

    SceUID heap_uid;
    SceKernelHeapCreateOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.uselock = 1;
    heap_uid = ksceKernelCreateHeap("pebbleHeap", 0x4000, &opt);

    if (heap_uid < 0)
        return SCE_KERNEL_START_FAILED;
    g_heap_uid = heap_uid;

    gui_init();

    guistate.ui_state = UI_WELCOME;
    guistate.view_state = VIEW_STACK;
    guistate.edit_mode = EDIT_NONE;
    load_hotkeys();

    SceUID thid = ksceKernelCreateThread("pebble", pebble_thread, 0x40, 0x40000, 0, 0, NULL);
    if (thid >= 0)
        ksceKernelStartThread(thid, 0, NULL);

    return SCE_KERNEL_START_SUCCESS;
}