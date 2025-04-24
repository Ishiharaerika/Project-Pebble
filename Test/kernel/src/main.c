#include "kernel.h"

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);
int (*ksceKernelSetPHWP)(SceUID pid, SceUInt32 a2, void *WVR, SceUInt32 WCR);
int (*ksceKernelSetPHBP)(SceUID pid, SceUInt32 a2, void *BVR, SceUInt32 BCR);

SceUID heap_uid = 0;
int8_t fb_now_idx = 0;
uint32_t *current_display_ptr;
TargetProcess g_target_process;
uint32_t *fb_bases[2] = {0, 0};
static tai_hook_ref_t g_framebufhook;

static int find_empty_slot(int start, int end)
{
    for (int i = start; i < end; ++i)
        if (guistate.breakpoints[i].type == SLOT_NONE)
            return i;
    return -1;
}

static void clear_slot(ActiveBKPTSlot *slot)
{
    if (slot)
    {
        memset(slot, 0, sizeof(*slot));
        slot->index = 0xFF;
    }
}

void kernel_debugger_on_create(void)
{
    g_target_process.main_module_id = ksceKernelGetProcessMainModule(g_target_process.pid);
    g_target_process.main_thread_id = ksceKernelGetProcessMainThread(g_target_process.pid);
    g_target_process.exception_thid = 0;
}

void kernel_debugger_init(void)
{
    g_target_process.pid = 0;
    g_target_process.main_module_id = 0;
    g_target_process.main_thread_id = 0;
    g_target_process.exception_thid = 0;
    guistate.gui_visible = false;
    guistate.ui_state = UI_WELCOME;
    guistate.view_state = VIEW_STACK;
    guistate.edit_mode = EDIT_NONE;
}

int kernel_set_hardware_breakpoint(uint32_t address)
{
    int index = find_empty_slot(0, SINGLE_STEP_SLOT);
    if (index < 0 || g_target_process.pid <= 0)
        return -1;
    uint32_t BCR = (1 << 0) | (0x3 << 1) | (0xF << 5) | (0x1 << 14) | (0x0 << 20);
    if (ksceKernelSetPHBP(g_target_process.pid, index, (void *)address, BCR) >= 0)
    {
        ActiveBKPTSlot *slot = &guistate.breakpoints[index];
        slot->pid = g_target_process.pid;
        slot->address = address;
        slot->index = index;
        slot->type = HW_BREAKPOINT;
        ksceKernelPrintf("Hardware breakpoint set for PID %#08X in slot %d\n", g_target_process.pid, index);
        return 0;
    }
    ksceKernelPrintf("Failed to set hardware breakpoint for PID %#08X\n", g_target_process.pid);
    return -1;
}

int kernel_set_watchpoint(uint32_t address, WatchPointBreakType type)
{
    int index = find_empty_slot(0, SINGLE_STEP_SLOT);
    if (index < 0 || g_target_process.pid <= 0)
        return -1;
    uint32_t WCR = (1 << 0) | (0x3 << 1) | (type << 3) | (0xF << 5) | (0x1 << 14);
    if (ksceKernelSetPHWP(g_target_process.pid, index, (void *)address, WCR) >= 0)
    {
        ActiveBKPTSlot *slot = &guistate.breakpoints[index];
        slot->pid = g_target_process.pid;
        slot->address = address;
        slot->index = index;
        slot->type = (type == BREAK_READ)    ? HW_WATCHPOINT_R
                     : (type == BREAK_WRITE) ? HW_WATCHPOINT_W
                                             : HW_WATCHPOINT_RW;
        ksceKernelPrintf("Watchpoint set for PID %#08X in slot %d\n", g_target_process.pid, index);
        return 0;
    }
    ksceKernelPrintf("Failed to set watchpoint for PID %#08X\n", g_target_process.pid);
    return -1;
}

int kernel_set_software_breakpoint(uint32_t address, SlotType type)
{
    if ((type != SW_BREAKPOINT_THUMB && type != SW_BREAKPOINT_ARM) || g_target_process.pid <= 0)
        return -1;
    int index = find_empty_slot(MAX_HW_BKPT, MAX_SLOT);
    if (index < 0)
        return -1;
    uint32_t bkpt_instruction = (type == SW_BREAKPOINT_THUMB) ? SW_THUMB : SW_ARM;
    uint8_t instruction_size = (type == SW_BREAKPOINT_THUMB) ? 2 : 4;
    uint32_t original_instruction = 0;
    if (ksceKernelCopyFromUserProc(g_target_process.pid, &original_instruction, (void *)address, instruction_size) < 0)
        return -1;
    int ret =
        ksceKernelCopyToUserProcTextDomain(g_target_process.pid, (void *)address, &bkpt_instruction, instruction_size);
    if (ret < 0)
    {
        ksceKernelCopyToUserProcTextDomain(g_target_process.pid, (void *)address, &original_instruction,
                                           instruction_size);
        return ret;
    }
    ActiveBKPTSlot *slot = &guistate.breakpoints[index];
    slot->pid = g_target_process.pid;
    slot->address = address;
    slot->index = index;
    slot->p_instruction = original_instruction;
    slot->type = type;
    ksceKernelPrintf("Software breakpoint set for PID %#08X in slot %d\n", g_target_process.pid, index);
    return ret;
}

int kernel_clear_breakpoint(int index)
{
    if (index < 0 || index >= MAX_SLOT)
        return -1;
    ActiveBKPTSlot *slot = &guistate.breakpoints[index];
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
        if (ksceKernelCopyToUserProcTextDomain(pid, (void *)slot->address, &slot->p_instruction, size) >= 0)
            clear_slot(slot);
    }
    return ret;
}

int kernel_list_breakpoints(ActiveBKPTSlot *dst)
{
    if (!dst)
        return -1;
    memcpy(dst, guistate.breakpoints, sizeof(ActiveBKPTSlot));
    return 0;
}

int kernel_get_registers(SceArmCpuRegisters *dst)
{
    if (!dst)
        return -1;
    memcpy(dst, &current_registers, sizeof(SceArmCpuRegisters));
    return 0;
}

int kernel_get_callstack(uint32_t *dst, int depth)
{
    if (!dst || depth <= 0)
        return -2;
    if (g_target_process.pid <= 0 || g_target_process.exception_thid <= 0 ||
        ksceKernelIsThreadDebugSuspended(g_target_process.exception_thid) <= 0)
        return ksceKernelPrintf("Let a breakpoint be triggered first to use GET_CALLSTACK.");
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
        if (ksceKernelCopyFromUserProc(g_target_process.pid, &saved_lr, (void *)(current_fp - 4), sizeof(saved_lr)) <
                0 ||
            ksceKernelCopyFromUserProc(g_target_process.pid, &saved_fp, (void *)current_fp, sizeof(saved_fp)) < 0)
            break;
        if (saved_lr == 0 || saved_fp == current_fp)
            break;
        call_stack_buffer[current_depth++] = saved_lr;
        current_fp = saved_fp;
    }
    memcpy(dst, &call_stack_buffer, current_depth * sizeof(uint32_t));
    return current_depth;
}

int kernel_get_modulelist(SceUID *user_modids, SceSize *user_num)
{
    if (g_target_process.pid <= 0 || !user_modids || !user_num)
    {
        ksceKernelPrintf("Attach to a process first to use GET_MODULELIST.");
        return -1;
    }
    return ksceKernelGetModuleList(g_target_process.pid, 0x7FFFFFFF, 1, user_modids, user_num) < 0 ? -1 : 0;
}

int kernel_get_moduleinfo(SceKernelModuleInfo *module_info)
{
    if (g_target_process.pid <= 0 || !module_info)
    {
        ksceKernelPrintf("Attach to a process first to use GET_MODULEINFO.");
        return -1;
    }
    return ksceKernelGetModuleInfo(g_target_process.pid, ksceKernelGetModuleIdByPid(g_target_process.pid), module_info);
}

int kernel_get_memblockinfo(const void *address, uint32_t info)
{
    SceUID memblok_uid = ksceKernelFindProcMemBlockByAddr(g_target_process.pid, address, 0);
    if (memblok_uid <= 0)
        return -1;
    return ksceKernelGetMemBlockType(memblok_uid, &info);
}

void kernel_suspend_process(void)
{
    if (g_target_process.pid <= 0)
        ksceKernelPrintf("Attach to a process first to use SUSPEND.");
    else
        ksceKernelSuspendProcess(g_target_process.pid, 0x1C);
}

void kernel_resume_process(void)
{
    if (g_target_process.exception_thid && ksceKernelIsThreadDebugSuspended(g_target_process.exception_thid) > 0)
    {
        ksceKernelChangeThreadSuspendStatus(g_target_process.exception_thid, 2);
        ksceKernelResumeProcess(g_target_process.pid);
    }
    else
        ksceKernelResumeProcess(g_target_process.pid);
}

static bool branch_condition(uint32_t cond, uint32_t cpsr)
{
    switch (cond)
    {
    case 0:
        return (cpsr & (1 << 30)) != 0;
    case 1:
        return (cpsr & (1 << 30)) == 0;
    case 2:
        return (cpsr & (1 << 29)) != 0;
    case 3:
        return (cpsr & (1 << 29)) == 0;
    case 4:
        return (cpsr & (1 << 31)) != 0;
    case 5:
        return (cpsr & (1 << 31)) == 0;
    case 6:
        return (cpsr & (1 << 28)) != 0;
    case 7:
        return (cpsr & (1 << 28)) == 0;
    case 8:
        return ((cpsr & (1 << 29)) != 0) && ((cpsr & (1 << 30)) == 0);
    case 9:
        return ((cpsr & (1 << 29)) == 0) || ((cpsr & (1 << 30)) != 0);
    case 10:
        return (((cpsr >> 31) & 1) == ((cpsr >> 28) & 1));
    case 11:
        return (((cpsr >> 31) & 1) != ((cpsr >> 28) & 1));
    case 12:
        return ((cpsr & (1 << 30)) == 0) && (((cpsr >> 31) & 1) == ((cpsr >> 28) & 1));
    case 13:
        return ((cpsr & (1 << 30)) != 0) || (((cpsr >> 31) & 1) != ((cpsr >> 28) & 1));
    case 14:
        return true;
    case 15:
        return false;
    }
    return false;
}

int kernel_single_step(void)
{
    if (g_target_process.pid <= 0 || g_target_process.exception_thid <= 0 ||
        ksceKernelIsThreadDebugSuspended(g_target_process.exception_thid) <= 0)
        return ksceKernelPrintf("Let a breakpoint be triggered first to use STEP.");
        
    uint32_t current_pc = current_registers.pc;
    uint32_t cpsr = current_registers.cpsr;
    uint32_t next_pc = 0;
    uint32_t instruction = 0;

    if (ksceKernelCopyFromUserProc(g_target_process.pid, &instruction, (void *)current_pc, 4) < 0)
        return -1;

    bool is_thumb = (cpsr & (1 << 5)) != 0;
    if (is_thumb)
    {
        uint16_t instr16 = instruction & 0xFFFF;
        bool is_32bit = (instr16 & 0xE000) == 0xE000 || (instr16 & 0xF800) == 0xF000 || (instr16 & 0xF800) == 0xF800;
        if (!is_32bit)
        {
            uint32_t instr_pc = current_pc + 2;
            if ((instr16 & 0xF800) == 0xE000)
            {
                int32_t offset = ((instr16 & 0x7FF) << 1);
                next_pc = instr_pc + ((offset & 0x800) ? (int32_t)(offset | 0xFFFFF000) : offset);
            }
            else if ((instr16 & 0xF000) == 0xD000 && (instr16 & 0x0F00) != 0x0F00 && (instr16 & 0x0F00) != 0x0E00)
            {
                uint32_t cond = (instr16 >> 8) & 0xF;
                int32_t offset = ((instr16 & 0xFF) << 1);
                next_pc = branch_condition(cond, cpsr)
                              ? (instr_pc + ((offset & 0x100) ? (int32_t)(offset | 0xFFFFFE00) : offset))
                              : instr_pc;
            }
            else
                next_pc = instr_pc;
        }
        else
        {
            uint32_t instr_pc = current_pc + 4;
            uint16_t instr_hi = instruction >> 16;

            if (((instr16 & 0xF800) == 0xF000) && ((instr_hi & 0xD000) == 0x8000))
            {
                uint32_t S = (instr16 >> 10) & 1;
                uint32_t J1 = (instr_hi >> 13) & 1;
                uint32_t J2 = (instr_hi >> 11) & 1;
                uint32_t imm10 = instr16 & 0x3FF;
                uint32_t imm11 = instr_hi & 0x7FF;
                int32_t offset = (S << 24) | (J1 << 23) | (J2 << 22) | (imm10 << 12) | (imm11 << 1);
                offset = S ? (int32_t)(offset | 0xFE000000) : offset;
                uint32_t cond = (instr16 >> 6) & 0xF;
                bool is_unconditional = (cond & 0xE) == 0xE || ((instr_hi >> 12) & 0x1);
                next_pc = is_unconditional || branch_condition(cond, cpsr) ? (instr_pc + offset) : instr_pc;
            }
            else
                next_pc = instr_pc;
        }
    }
    else
    {
        uint32_t instr_pc = current_pc + 8;
        uint32_t cond = instruction >> 28;
        bool condition_met = branch_condition(cond, cpsr);
        if (condition_met && (instruction & 0x0E000000) == 0x0A000000)
        {
            int32_t offset = (instruction & 0x00FFFFFF) << 2;
            next_pc = instr_pc + ((offset & 0x02000000) ? (int32_t)(offset | 0xFC000000) : offset);
        }
        else
            next_pc = current_pc + 4;
    }

    uint32_t BCR = (1 << 0) | (0x3 << 1) | (0xF << 5) | (0x1 << 14) | (0x0 << 20);
    int ret = ksceKernelSetPHBP(g_target_process.pid, SINGLE_STEP_SLOT, (void *)next_pc, BCR);
    if (ret >= 0)
    {
        ActiveBKPTSlot *slot = &guistate.breakpoints[SINGLE_STEP_SLOT];
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
    if (!src_addr || !user_dst || !size || g_target_process.pid <= 0)
        return -1;
    
    return ksceKernelCopyFromUserProc(g_target_process.pid, user_dst, src_addr, size);
}

int kernel_write_memory(void *dst, const void *user_modification, SceSize memwrite_len)
{
    if (!dst || !user_modification || !memwrite_len || g_target_process.pid <= 0 || heap_uid < 0)
        return -1;

    void *buf = ksceKernelAllocHeapMemory(heap_uid, memwrite_len);
    if (!buf)
        return -1;

    memcpy(buf, user_modification, memwrite_len);
    int ret = -1;
    uint32_t mem_type = 0;

    if (kernel_get_memblockinfo(dst, (uint32_t)&mem_type) >= 0)
    {
        if (mem_type & SCE_KERNEL_MEMBLOCK_TYPE_USER_RX)
            ret = ksceKernelCopyToUserProcTextDomain(g_target_process.pid, dst, buf, memwrite_len);
        else
            ret = ksceKernelCopyToUserProc(g_target_process.pid, dst, buf, memwrite_len);
    }

    ksceKernelFreeHeapMemory(heap_uid, buf);
    return ret;
}

int ksceDisplaySetFrameBufInternal_patched(int head, int index, const SceDisplayFrameBuf *pParam, int sync)
{
    if (!head || !g_target_process.pid || !pParam || !pParam->base || (index && isExclusive))
        return TAI_CONTINUE(int, g_framebufhook, head, index, pParam, sync);
    if (guistate.gui_visible && fb_bases[0] && fb_bases[1])
    {
        if (!ksceKernelLockMutex(pebble_mtx_uid, 1, NULL))
        {
            int pitch = pParam->pitch;
            int width = pParam->width < 960 ? pParam->width : 960;
            int height = pParam->height < 544 ? pParam->height : 544;
            current_display_ptr = fb_bases[1 - fb_now_idx];
            for (int i = 0; i < height; i++)
            {
                uintptr_t dst_addr = (uintptr_t)((uint32_t *)pParam->base + i * pitch);
                const void *src_addr = (const void *)(current_display_ptr + i * 960);
                int copy_size = width * sizeof(uint32_t);
                ksceKernelMemcpyKernelToUser((void *)dst_addr, src_addr, copy_size);
            }
            fb_now_idx = 1 - fb_now_idx;
            ksceKernelUnlockMutex(pebble_mtx_uid, 1);
        }
    }
    return TAI_CONTINUE(int, g_framebufhook, head, index, pParam, sync);
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(void)
{
    if (module_get_export_func(0x10005, "SceProcessmgr", PROCESSMGR_NID, SETPHBP_NID, (uintptr_t *)&ksceKernelSetPHBP) <
            0 ||
        module_get_export_func(0x10005, "SceProcessmgr", PROCESSMGR_NID, SETPHWP_NID, (uintptr_t *)&ksceKernelSetPHWP) <
            0)
        return SCE_KERNEL_START_FAILED;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    int ret = taiHookFunctionExportForKernel(0x10005, &g_framebufhook, "SceDisplay", 0x9FED47AC, 0x16466675,
                                             (const void *)ksceDisplaySetFrameBufInternal_patched);
#pragma GCC diagnostic pop
    if (ret < 0)
        return SCE_KERNEL_START_FAILED;

    load_hotkeys();
    kernel_debugger_init();

    SceUID thid = ksceKernelCreateThread("pebble", pebble_thread, 0x40, 0x4000, 0, 0, NULL);
    if (thid <= 0)
    {
        taiHookReleaseForKernel(ret, g_framebufhook);
        return SCE_KERNEL_START_FAILED;
    }
    else
        ksceKernelStartThread(thid, 0, NULL);

    SceKernelHeapCreateOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.uselock = 1;
    heap_uid = ksceKernelCreateHeap("pebbleHeap", 0x4000, &opt);

    if (heap_uid <= 0)
    {
        ksceKernelDeleteThread(thid);
        taiHookReleaseForKernel(ret, g_framebufhook);
        return SCE_KERNEL_START_FAILED;
    }
    memset(guistate.breakpoints, 0, sizeof(ActiveBKPTSlot));
    for (int i = 0; i < MAX_SLOT; ++i)
        guistate.breakpoints[i].index = 0xFF;
    return SCE_KERNEL_START_SUCCESS;
}