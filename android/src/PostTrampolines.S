// Detours that see the result (PostTrampolines.S): each saves x0-x8 and q0-q7, calls the original function with
// them (blr, on our own 256-byte frame: only for functions without stack arguments), then calls
// splitscreen_on_post(index, saved arguments, x0, x1, q0's low 64 bits) and returns the original's x0, x1 and q0.

    .text

.macro PTRAMPOLINE i
    .p2align 2
    .type splitscreen_ptrampoline_\i, %function
splitscreen_ptrampoline_\i:
    sub sp, sp, #256
    stp x29, x30, [sp]
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
    adrp x16, splitscreen_poriginals
    add x16, x16, :lo12:splitscreen_poriginals
    ldr x16, [x16, #(\i * 8)]
    blr x16
    stp x0, x1, [sp, #224]
    str q0, [sp, #240]
    mov x2, x0
    mov x3, x1
    fmov x4, d0
    mov x0, #\i
    add x1, sp, #16
    bl splitscreen_on_post
    ldr q0, [sp, #240]
    ldp x0, x1, [sp, #224]
    ldp x29, x30, [sp]
    add sp, sp, #256
    ret
    .size splitscreen_ptrampoline_\i, . - splitscreen_ptrampoline_\i
.endm

PTRAMPOLINE 0
PTRAMPOLINE 1
PTRAMPOLINE 2
PTRAMPOLINE 3
PTRAMPOLINE 4
PTRAMPOLINE 5
PTRAMPOLINE 6
PTRAMPOLINE 7
PTRAMPOLINE 8
PTRAMPOLINE 9
PTRAMPOLINE 10
PTRAMPOLINE 11
PTRAMPOLINE 12
PTRAMPOLINE 13
PTRAMPOLINE 14
PTRAMPOLINE 15

    .section .data.rel.ro, "aw"
    .p2align 3
    .globl splitscreen_ptrampolines
    .hidden splitscreen_ptrampolines
splitscreen_ptrampolines:
    .quad splitscreen_ptrampoline_0
    .quad splitscreen_ptrampoline_1
    .quad splitscreen_ptrampoline_2
    .quad splitscreen_ptrampoline_3
    .quad splitscreen_ptrampoline_4
    .quad splitscreen_ptrampoline_5
    .quad splitscreen_ptrampoline_6
    .quad splitscreen_ptrampoline_7
    .quad splitscreen_ptrampoline_8
    .quad splitscreen_ptrampoline_9
    .quad splitscreen_ptrampoline_10
    .quad splitscreen_ptrampoline_11
    .quad splitscreen_ptrampoline_12
    .quad splitscreen_ptrampoline_13
    .quad splitscreen_ptrampoline_14
    .quad splitscreen_ptrampoline_15

    .section .note.GNU-stack, "", %progbits
