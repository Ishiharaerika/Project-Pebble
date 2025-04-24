.section .text
.global handler_asm_pabt, handler_asm_dabt, handler_asm_undef
.arch armv7-a
.align 4
.syntax unified

.arm

@ PABT handler
asm_pabt:
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

.section .rodata
handler_asm_pabt:
    .word asm_pabt
handler_asm_dabt:
    .word asm_dabt
handler_asm_undef:
    .word asm_undef