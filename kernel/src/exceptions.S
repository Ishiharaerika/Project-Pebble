.section .text
.global handler_asm
.arch armv7-a
.align 4
.syntax unified

.arm
handler_asm:
  .word 0x0
  .word 0x0

  @ Call exception_handler
  mov   r4, r0
  stmfd sp!, {r4, lr}
  cpsie i
  blx   exception_handler
  cpsid i
  ldmfd sp!, {r4, lr}
  mov   r0, r4
  mov   r1, #0

  @ Clear fault registers
  mcr   p15, 0, r1, c5, c0, 0
  mcr   p15, 0, r1, c5, c0, 1
  mcr   p15, 0, r1, c5, c1, 0
  mcr   p15, 0, r1, c5, c1, 1
  mcr   p15, 0, r1, c6, c0, 0
  mcr   p15, 0, r1, c6, c0, 2

  @ Restore system registers
  ldr   r1, [r0, #0x60]
  mcr   p15, 0, r1, c13, c0, 4
  ldr   r1, [r0, #0x5c]
  mcr   p15, 0, r1, c13, c0, 3

  @ Restore MMU context registers
  ldr   r1, [r0, #0x64]
  ldr   r2, [r0, #0x54]
  mov   r3, #0
  mcr   p15, 0, r3, c13, c0, 1
  isb   sy
  mcr   p15, 0, r1, c2, c0, 1
  isb   sy
  mcr   p15, 0, r2, c13, c0, 1

  @ Restore memory access controls
  ldr   r1, [r0, #0x6c]
  mcr   p15, 0, r1, c3, c0, 0
  ldr   r1, [r0, #0x80]
  mcr   p15, 0, r1, c7, c4, 0

  @ Check for FP registers
  ldr   r1, [r0, #0x40]
  cmp   r1, #0
  beq   Lno_fp

  @ Restore FP registers
  ldr   r2, [r0, #0x50]
  ldr   r3, [r0, #0x4c]
  vmsr  fpexc, r2
  vmsr  fpscr, r3
  add   r2, r0, #0x100
  vldmia r2!, {d0-d15}
  vldmia r2!, {d16-d31}

Lno_fp:
  @ Complete restoring
  ldr   r1, [r0, #0x48]
  mcr   p15, 0, r1, c1, c0, 2
  ldr   r1, [r0, #0x3C]
  cps   #0x17
  mov   r12, r0
  ldmia r12!, {r0-r11}
  add   r12, r12, #4
  ldmia r12, {sp,lr}^
  sub   r12, r12, #0x34

  @ Prepare for exception return
  ldr   lr, [r12, #0x44]
  str   lr, [sp, #-4]!
  ldr   lr, [r12, #0x3C]
  str   lr, [sp, #-4]!
  ldr   r12, [r12, #0x30]
  clrex
  rfefd sp!
