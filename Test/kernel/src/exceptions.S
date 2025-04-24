.section .text
.global asm_pabt, asm_dabt, asm_undef
.arch armv7-a
.align 4
.syntax unified

.arm

@ PABT handler
asm_pabt:
    .word 0x0
    .word 0x0
    push {r0-r5, lr}
    
    @ Check if exception occurred in user mode
    mrs r0, spsr
    and r0, r0, #0x1F
    cmp r0, #0x10  @ User mode
    bne pabt_not_user
    
    @ Save user context
    mov r5, #1  @ PABT code
    
    @ Get current registers for the thread that caused the exception
    bl exception_handler
    
    @ Return from exception
    pop {r0-r5, pc}
    
pabt_not_user:
    @ Not handling kernel exceptions
    pop {r0-r5, pc}

@ DABT handler
asm_dabt:
    .word 0x0
    .word 0x0
    push {r0-r5, lr}
    
    @ Check if exception occurred in user mode
    mrs r0, spsr
    and r0, r0, #0x1F
    cmp r0, #0x10  @ User mode
    bne dabt_not_user
    
    @ Save user context
    mov r5, #0  @ DABT code
    
    @ Get current registers for the thread that caused the exception
    bl exception_handler
    
    @ Return from exception
    pop {r0-r5, pc}
    
dabt_not_user:
    @ Not handling kernel exceptions
    pop {r0-r5, pc}

@ Undefined instruction handler
asm_undef:
    .word 0x0
    .word 0x0
    push {r0-r5, lr}
    
    @ Check if exception occurred in user mode
    mrs r0, spsr
    and r0, r0, #0x1F
    cmp r0, #0x10  @ User mode
    bne undef_not_user
    
    @ Save user context
    mov r5, #2  @ Undef code
    
    @ Get current registers for the thread that caused the exception
    bl exception_handler
    
    @ Return from exception
    pop {r0-r5, pc}

undef_not_user:
    @ Not handling kernel exceptions
    pop {r0-r5, pc}