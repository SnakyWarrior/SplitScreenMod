// Detours for functions whose signature is not known: each saves every argument register (x0-x8, q0-q7), calls
// splitscreen_on_call(index, saved registers, caller's frame pointer, return address), restores the registers
// (the C++ side may change the saved ones) and jumps to the original function, which returns to the caller.

    .text

.macro TRAMPOLINE i
    .globl splitscreen_trampoline_\i
    .hidden splitscreen_trampoline_\i
    .type splitscreen_trampoline_\i, %function
    .p2align 2
splitscreen_trampoline_\i:
    stp x29, x30, [sp, #-240]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    mov x0, #\i
    add x1, sp, #16
    ldr x2, [sp]
    ldr x3, [sp, #8]
    bl splitscreen_on_call
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x8, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #240
    adrp x16, splitscreen_originals
    add x16, x16, :lo12:splitscreen_originals
    ldr x16, [x16, #(\i * 8)]
    br x16
    .size splitscreen_trampoline_\i, . - splitscreen_trampoline_\i
.endm

TRAMPOLINE 0
TRAMPOLINE 1
TRAMPOLINE 2
TRAMPOLINE 3
TRAMPOLINE 4
TRAMPOLINE 5
TRAMPOLINE 6
TRAMPOLINE 7

    .section .note.GNU-stack, "", %progbits
