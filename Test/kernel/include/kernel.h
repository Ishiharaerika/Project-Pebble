#pragma once

#include <taihen.h>
#include <stdbool.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/display.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/excpmgr.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/kernel/processmgr.h>

#define UI_WIDTH 960 // ToDo: Needs adjust!!!
#define UI_HEIGHT 544 // ToDo: Needs adjust!!!

#define EXCEPTION_NOT_HANDLED 2
#define MAX_CALL_STACK_DEPTH 16
#define MAX_SLOT 16
#define MAX_HW_BKPT 4
#define SINGLE_STEP_SLOT (MAX_HW_BKPT - 1)
#define SW_THUMB 0xBE00
#define SW_ARM 0xE1200070
// #define THREADMGR_NID 0xE2C40624 //Wrong for 3.63+?
// #define SETTHBP_NID 0x385831A1 //Wrong for 3.63+?
#define PROCESSMGR_NID 0xEB1F8EF7
#define SYSMEM_NID 0x37FE725A
#define SETPHWP_NID 0xB2421F93
#define SETPHBP_NID 0x597E6D2C
#define MEMBLKINFO_NID 0x4010AD65

#define DEFAULT_SHOW_GUI (SCE_CTRL_LTRIGGER | SCE_CTRL_START)
#define DEFAULT_AREA_SELECT SCE_CTRL_SELECT
#define DEFAULT_MEMORY_MODE (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)
#define DEFAULT_CANCEL SCE_CTRL_CROSS
#define DEFAULT_CONFIRM SCE_CTRL_CIRCLE
#define HOTKEY_PATH "ux0:data/pebbleHotkey.txt"
#define CLAMP(x, m, M) ((x) <= (m) ? (m) : (x) >= (M) ? (M) : (x))

typedef enum
{
    SLOT_NONE,
    SW_BREAKPOINT_THUMB,
    SW_BREAKPOINT_ARM,
    HW_BREAKPOINT,
    HW_WATCHPOINT_R,
    HW_WATCHPOINT_W,
    HW_WATCHPOINT_RW,
    SINGLE_STEP_HW_BREAKPOINT
} SlotType;

typedef enum
{
    BREAK_READ = 1,
    BREAK_WRITE = 2,
    BREAK_READ_WRITE = 3
} WatchPointBreakType;

typedef struct
{
    SceUID pid;
    uint32_t address;
    uint8_t index;
    uint32_t p_instruction; // Previous instruction
    SlotType type;
} ActiveBKPTSlot;

typedef struct
{
    SceUID pid;
    SceUID main_module_id;
    SceUID main_thread_id;
    SceUID exception_thid;
} TargetProcess;

typedef enum
{
    UI_WELCOME,
    UI_MEMVIEW,
    UI_FEATURES,
    MEMVIEW_HEX,
    MEMVIEW_REGS,
    MEMVIEW_STACK,
    UI_FEATURE_HW_BREAK,
    UI_FEATURE_WATCH,
    UI_FEATURE_SW_BREAK,
    UI_FEATURE_CLEAR,
    UI_FEATURE_SUSPEND,
    UI_FEATURE_RESUME,
    UI_FEATURE_STEP,
    UI_FEATURE_HOTKEYS
} UIState;

typedef enum
{
    VIEW_STACK,
    VIEW_CALLSTACK,
    VIEW_BREAKPOINTS
} StackViewState;

typedef enum
{
    MEM_LAYOUT_8BIT,
    MEM_LAYOUT_16BIT,
    MEM_LAYOUT_32BIT
} MemLayout;

typedef enum
{
    EDIT_NONE,
    EDIT_ADDRESS,
    EDIT_VALUE
} EditMode;

typedef struct
{
    const char *format;
    int bytes;
} MemLayoutInfo;

typedef struct
{
    uint32_t show_gui;
    uint32_t area_select;
    uint32_t cancel;
    uint32_t confirm;
} HotkeyConfig;

typedef struct
{
    int edit_offset, cursor_column;
    EditMode edit_mode;
    MemLayout mem_layout;
    HotkeyConfig hotkeys;
    SceArmCpuRegisters regs;
    StackViewState view_state;
    SceKernelModuleInfo modinfo;
    UIState ui_state, active_area;
    ActiveBKPTSlot breakpoints[MAX_SLOT];
    uint8_t modified_value[4], cached_mem[256];
    bool gui_visible, has_active_bp;
    uint32_t addr, base_addr, modified_addr, edit_feature, stack[64], callstack[MAX_CALL_STACK_DEPTH], stack_size, callstack_size;
} State;

extern State guistate;
extern SceUID evtflag;
extern uint8_t buf_index;
extern SceUID pebble_mtx_uid;
extern uint32_t lowest_vaddr;
extern uint32_t *fb_bases[2];
extern uint32_t highest_vaddr;
extern uint32_t *userframe_base;
extern TargetProcess g_target_process;
extern SceArmCpuRegisters current_registers;

void draw_gui(void);
void load_hotkeys(void);
int pebble_thread(SceSize args, void *argp);

// main.c
extern int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);
// extern int (*ksceKernelSetTHBP)(SceUID thid, SceUInt32 a2, void *BVR, SceUInt32 BCR);
extern int (*ksceKernelSetPHWP)(SceUID pid, SceUInt32 a2, void *WVR, SceUInt32 WCR);
extern int (*ksceKernelSetPHBP)(SceUID pid, SceUInt32 a2, void *BVR, SceUInt32 BCR);

//bool kernel_check_event(void);
void kernel_debugger_on_create(void);
void kernel_debugger_init(void);
int kernel_set_hardware_breakpoint(uint32_t address);
int kernel_set_watchpoint(uint32_t address, WatchPointBreakType type);
int kernel_set_software_breakpoint(uint32_t address, SlotType type);
int kernel_clear_breakpoint(int index);
int kernel_list_breakpoints(ActiveBKPTSlot *user_dst);
int kernel_get_registers(SceArmCpuRegisters *user_dst);
int kernel_get_callstack(uint32_t *user_dst, int depth);
int kernel_get_modulelist(SceUID *user_modids, SceSize *user_num);
int kernel_get_moduleinfo(SceKernelModuleInfo *module_info);
int kernel_get_memblockinfo(const void *address, uint32_t *info);
void kernel_suspend_process(void);
void kernel_resume_process(void);
int kernel_single_step(void);
int kernel_read_memory(const void *src_addr, void *user_dst, SceSize size);
int kernel_write_memory(uint32_t user_dst, const void *user_modification, SceSize memwrite_len);
void kernel_get_userinfo(SceUID PID_user, SceUID pebble_mtx_uid_user, uint32_t *fb_base0_user, SceUID evtflag_user);
int register_handler(void);