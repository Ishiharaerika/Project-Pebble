#include <psp2kern/kernel/excpmgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr/debugger.h>

#include "kernel.h"

extern void handler_asm(void);

int exception_handler(void) {
  SceKernelThreadContextInfo info;
  ThreadCpuRegisters temp_registers;
  int ret;

  if ((ret = ksceKernelGetThreadContextInfo(&info)) < 0) return EXCEPTION_NOT_HANDLED;

  if (g_target_process.pid != info.process_id) return EXCEPTION_NOT_HANDLED;

  g_target_process.exception_thid = info.thread_id;

  if ((ret = ksceKernelGetThreadCpuRegisters(info.thread_id, &temp_registers)) < 0)
    return EXCEPTION_NOT_HANDLED;

  memcpy(&g_saved_context, &temp_registers.user, sizeof(ExceptionContext));

  if (g_active_slot[SINGLE_STEP_SLOT].type == SINGLE_STEP_HW_BREAKPOINT &&
      g_active_slot[SINGLE_STEP_SLOT].address == temp_registers.user.pc) {
    kernel_clear_breakpoint(SINGLE_STEP_SLOT);
  }

  ksceKernelChangeThreadSuspendStatus(info.thread_id, 0x1002);
  return EXCEPTION_NOT_HANDLED;
}

void register_handler(void) {
  ksceExcpmgrRegisterHandler(SCE_EXCP_PABT, 5, (void *)handler_asm);
  ksceExcpmgrRegisterHandler(SCE_EXCP_DABT, 5, (void *)handler_asm);
  ksceExcpmgrRegisterHandler(SCE_EXCP_UNDEF_INSTRUCTION, 5, (void *)handler_asm);
}