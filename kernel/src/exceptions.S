.section .text
.global handler_asm
.arch armv7-a
.align 4
.syntax unified
.thumb

handler_asm:
push {r0-r3, lr}
bl exception_handler
pop {r0-r3, pc}