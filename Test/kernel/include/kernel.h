#pragma once

#include <psp2kern/ctrl.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/excpmgr.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/processmgr.h>
//#include <psp2kern/kernel/proc_event.h>

#define EXCEPTION_NOT_HANDLED 2
#define MAX_CALL_STACK_DEPTH 32
#define MAX_SLOT 20
#define MAX_HW_BKPT 4
#define SINGLE_STEP_SLOT (MAX_HW_BKPT - 1)
#define SW_THUMB 0xBE00
#define SW_ARM 0xE1200070
//#define THREADMGR_NID 0xE2C40624
#define PROCESSMGR_NID 0x7A69DE86
#define SYSMEM_NID 0x37FE725A
//#define SETTHBP_NID 0x385831A1
//#define GETTHBP_NID 0x453B764A
#define SETPHWP_NID 0x54D7B16A
//#define GETPHWP_NID 0xC55BF6C3
#define SETPHBP_NID 0x59FA3216
//#define GETPHBP_NID 0xA9C20202
#define MEMBLKINFO_NID 0x4010AD65

#define DEFAULT_SHOW_GUI (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)
#define DEFAULT_AREA_SELECT SCE_CTRL_SELECT
#define DEFAULT_EDIT_MODE SCE_CTRL_CROSS
#define DEFAULT_MEMORY_MODE (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)
#define DEFAULT_CANCEL SCE_CTRL_CIRCLE
#define DEFAULT_CONFIRM SCE_CTRL_CROSS
#define HOTKEY_PATH "ux0:data/pebbleHotkey.txt"
#define CLAMP(x, m, M) ((x) <= (m) ? (m) : (x) >= (M) ? (M) : (x))

typedef enum {
  SLOT_NONE = 0,
  SW_BREAKPOINT_THUMB,
  SW_BREAKPOINT_ARM,
  HW_BREAKPOINT,
  HW_WATCHPOINT_R,
  HW_WATCHPOINT_W,
  HW_WATCHPOINT_RW,
  SINGLE_STEP_HW_BREAKPOINT
} SlotType;

typedef enum {
   BREAK_READ = 1,
   BREAK_WRITE = 2,
   BREAK_READ_WRITE = 3
} WatchPointBreakType;

typedef struct {
  SceUID pid;
  uint32_t address;
  uint8_t index;
  uint32_t p_instruction; //Previous instruction
  SlotType type;
} ActiveBKPTSlot;

typedef struct {
  SceUID pid;
  SceUID main_module_id;
  SceUID main_thread_id;
  SceUID exception_thid;
} TargetProcess;

typedef enum {
  UI_WELCOME, UI_MEMVIEW, UI_FEATURES,
  MEMVIEW_HEX, MEMVIEW_REGS, MEMVIEW_STACK,
  UI_FEATURE_HW_BREAK, UI_FEATURE_WATCH, UI_FEATURE_SW_BREAK, 
  UI_FEATURE_CLEAR, UI_FEATURE_SUSPEND, UI_FEATURE_RESUME,
  UI_FEATURE_STEP, UI_FEATURE_HOTKEYS
} UIState;

typedef enum {
  VIEW_STACK, VIEW_CALLSTACK, VIEW_BREAKPOINTS
} StackViewState;

typedef enum { 
  MEM_LAYOUT_8BIT, MEM_LAYOUT_16BIT, MEM_LAYOUT_32BIT 
} MemLayout;

typedef enum { 
  EDIT_NONE, EDIT_ADDRESS, EDIT_VALUE
} EditMode;

typedef struct {
  const char *format;
  int bytes;
} MemLayoutInfo;

typedef struct {
  int show_gui;
  int area_select;
  int edit;
  int cancel;
  int confirm;
} HotkeyConfig;

typedef struct {
  UIState ui_state;
  UIState active_area;
  StackViewState view_state;
  uint32_t addr, base_addr, modified_addr, edit_feature;
  uint8_t modified_value[4];
  int edit_offset;
  MemLayout mem_layout;
  SceUID pid;
  SceKernelModuleInfo modinfo;
  SceArmCpuRegisters regs;
  HotkeyConfig hotkeys;
  ActiveBKPTSlot breakpoints[16];
  uint32_t stack[64], callstack[16];
  int stack_size, callstack_size;
  EditMode edit_mode;
} State;

typedef struct SceKernelMemBlockInfo {
	SceSize size;
	void *mappedBase;
	SceSize mappedSize;
	int memoryType;
	SceUInt32 access;
	SceKernelMemBlockType type;
} SceKernelMemBlockInfo;

extern TargetProcess g_target_process;
extern ActiveBKPTSlot g_active_slot[MAX_SLOT];
extern SceThreadCpuRegisters all_registers;
extern SceArmCpuRegisters current_registers;
extern SceUID g_heap_uid;
extern SceUID g_breakpoint_triggered;
extern State guistate;

//gui.c
int pebble_thread(SceSize args, void *argp);
void load_hotkeys(void);

//main.c
extern int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);
//extern int (*ksceKernelSetTHBP)(SceUID thid, SceUInt32 a2, void *BVR, SceUInt32 BCR);
extern int (*ksceKernelSetPHWP)(SceUID pid, SceUInt32 a2, void *WVR, SceUInt32 WCR);
extern int (*ksceKernelSetPHBP)(SceUID pid, SceUInt32 a2, void *BVR, SceUInt32 BCR);
extern int (*sceKernelGetMemBlockInfoByAddr)(void *base, SceKernelMemBlockInfo *info);

int kernel_debugger_attach(SceUID pid);
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
int kernel_suspend_process(void);
int kernel_resume_process(void);
int kernel_single_step(void);
int kernel_read_memory(const void *src_addr, void *user_dst, SceSize size);
int kernel_write_memory(void *user_dst, const void *user_modification, SceSize memwrite_len);
int kernel_write_instruction(void *user_dst, const void *user_modification, SceSize memwrite_len);
int register_handler(void);