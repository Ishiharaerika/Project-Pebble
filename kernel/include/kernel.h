#pragma once

#include <psp2kern/types.h>
#include <psp2kern/kernel/excpmgr.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/processmgr.h>

#define EXCEPTION_NOT_HANDLED 2
#define MAX_CALL_STACK_DEPTH 32
#define MAX_SLOT 20
#define MAX_HW_BKPT 4
#define SINGLE_STEP_SLOT (MAX_HW_BKPT - 1)
#define SW_THUMB 0xBE00
#define SW_ARM 0xE1200070
#define THREADMGR_NID 0xE2C40624
#define PROCESSMGR_NID 0x7A69DE86
#define SETTHBP_NID 0x385831A1
#define SETPHWP_NID 0x54D7B16A
#define SETPHBP_NID 0x59FA3216

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

extern TargetProcess g_target_process;
extern ActiveBKPTSlot g_active_slot[MAX_SLOT];
extern SceThreadCpuRegisters all_registers;
extern SceArmCpuRegisters current_registers;

int kernel_debugger_attach(SceUID pid);
int kernel_set_hardware_breakpoint(SceUID pid, uint32_t address);
int kernel_set_watchpoint(SceUID pid, uint32_t address, WatchPointBreakType type);
int kernel_set_software_breakpoint(SceUID pid, uint32_t address, SlotType type);
int kernel_clear_breakpoint(int index);
int kernel_list_breakpoints(ActiveBKPTSlot *user_dst);
int kernel_get_registers(SceArmCpuRegisters *user_dst);
int kernel_get_callstack(uint32_t *user_dst, int depth);
int kernel_suspend_process(SceUID pid);
int kernel_resume_process(SceUID pid);
int kernel_single_step(void);
int register_handler(void);
