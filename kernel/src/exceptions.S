.balign 0x10
.text
.cpu cortex-a9
.arch armv7-a
.syntax unified
.arm
.fpu neon

.macro exception_handler_common exc_type, lr_offset
    .word 0
    .word 0
    @   stage_1
    push    {r0}
    ldr     r0, =stage_1
    bl      ksceKernelPrintf
    pop     {r0}

    sub sp, sp, #8
    push {r0-r3, ip, lr}
    sub sp, sp, #0x20
	
    @ Setup jump context
    sub     r0, lr, #\lr_offset      @ DABT is PC-8
    str     r0, [sp, #0x38]          @ PC
    mrs     r0, cpsr
    str     r0, [sp, #0x3C]          @ CPSR
	
    mrs     r0, spsr
    and     r0, #0x1F
    cmp     r0, #0x10
    bne     1f                       @ Skip if not user mode

	@   stage_2
    push    {r0}
    ldr     r0, =stage_2
    bl      ksceKernelPrintf
    pop     {r0}

    str     r0, [sp]                 @ Save spsr value
    mov     r3, sp                   @ Save pointer to original stack
    cps     #0x1F                    @ Switch to system mode
    mov     r2, sp                   @ Save system mode sp
    mrc     p15, #0, r1, c13, c0, #3 @ TPIDRURO
    str     r1, [r3, #0x4]           @ Save TPIDRURO to original stack
    mrc     p15, #0, r1, c13, c0, #4 @ TPIDRPRW
    ldr     sp, [r1, #0x30]          @ Set kernel stack

	@   stage_3
    push    {r0}
    ldr     r0, =stage_3
    bl      ksceKernelPrintf
    pop     {r0}

    @ Setup syscall frame and registers
    sub     sp, #0x60
    str     sp, [r1, #0x44]
    str     r2, [sp, #0x50]          @ Save system mode sp
    str     lr, [sp, #0x54]          @ Save system mode lr
    ldr     r2, [r3, #0x38]          @ PC from abort stack
    ldr     lr, [r3]                 @ SPSR from abort stack
    str     r2, [sp, #0x58]
    str     lr, [sp, #0x5C]
    
    mov     r2, #0
    vmrs    lr, fpscr
    str     r2, [sp, #0x38]
    str     lr, [sp, #0x3C]
    ldr     r0, [sp, #0x50]          @ Get system mode sp
    add     r0, #0x28
    ldm     r0, {r0-r3}              @ Load pushed registers
    stm     sp, {r0-r12}             @ Save all registers to frame

    mov     r3, #0
    mrc     p15, #0, r2, c13, c0, #2 @ TPIDRURW
    str     r3, [sp, #0x40]
    str     r2, [sp, #0x44]
    mov     r2, #1
    mcr     p15, #0, r2, c13, c0, #3 @ TPIDRURO

	@   stage_4
    push    {r0}
    ldr     r0, =stage_4
    bl      ksceKernelPrintf
    pop     {r0}

    @ Save FPU registers
    vpush   {d0-d15}
    vpush   {d16-d31}

    @ Call handler with DFAR value
    cpsie   i
    mov     r0, #\exc_type
    mrc     p15, #0, r1, c6, c0, #0  @ Get DFAR
    dmb     sy
    dsb     sy
    blx     exception_handler
    dmb     sy
    dsb     sy
    cpsid   i

	@   stage_5
    push    {r0}
    ldr     r0, =stage_5
    bl      ksceKernelPrintf
    pop     {r0}

    @ Restore state
    vpop    {d16-d31}
    vpop    {d0-d15}
    mrc     p15, #0, r1, c13, c0, #4 @ TPIDRPRW
    mov     r2, #0
    str     r2, [r1, #0x44]          @ Clear syscall frame pointer
    ldr     r1, [sp, #0x3C]
    vmsr    fpscr, r1
    ldr     lr, [sp, #0x54]          @ Restore system mode lr
    ldr     sp, [sp, #0x50]          @ Restore system mode sp
    cps     #0x17                    @ Switch back to abort mode
    ldr     r1, [r3, #0x4]           @ Get saved TPIDRURO
    mcr     p15, #0, r1, c13, c0, #3 @ Restore TPIDRURO

	@   stage_6
    push    {r0}
    ldr     r0, =stage_6
    bl      ksceKernelPrintf
    pop     {r0}

    @ Exception was handled, prepare to exit
    mrs     r0, spsr                 @ Load SPSR of abort mode
    str     r0, [sp, #0x3C]          @ Store as CPSR for return

1:
    @ Return from exception
    add     sp, #0x20
    pop     {r0-r3, ip, lr}
    rfeia   sp!
.endm

@ Prefetch Abort handler
.globl asm_pabt
.type  asm_pabt, %function
asm_pabt:
    .word 0
    .word 0
    exception_handler_common 3, 4    @ PABT is PC-4

.globl asm_dabt
.type  asm_dabt, %function
asm_dabt:
    .word 0
    .word 0
    exception_handler_common 4, 8    @ DABT is PC-8

@ Undefined Instruction handler
.globl asm_undef
.type  asm_undef, %function
asm_undef:
    .word 0
    .word 0
    exception_handler_common 1, 4    @ UNDEF is PC-4

.section .rodata
stage_1:
    .asciz "ASM Stage 1111\n"

stage_2:
    .asciz "ASM Stage 2222\n"

stage_3:
    .asciz "ASM Stage 3333\n"

stage_4:
    .asciz "ASM Stage 4444\n"

stage_5:
    .asciz "ASM Stage 5555\n"

stage_6:
    .asciz "ASM Stage 6666\n"