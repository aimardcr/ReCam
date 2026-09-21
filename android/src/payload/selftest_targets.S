// Hook targets with deliberately chosen prologues, for the payload self-test.

    .text

// ---------------------------------------------------------------- plain prologue
    .globl  recam_test_plain
    .type   recam_test_plain, %function
    .balign 16
recam_test_plain:
    sub     sp, sp, #32
    stp     x29, x30, [sp, #16]
    add     x29, sp, #16
    mov     w1, w0
    add     w0, w1, #1
    ldp     x29, x30, [sp, #16]
    add     sp, sp, #32
    ret
    .size   recam_test_plain, .-recam_test_plain

// ---------------------------------------------------------------- BTI landing pad
    .globl  recam_test_bti
    .type   recam_test_bti, %function
    .balign 16
recam_test_bti:
    bti     c
    sub     sp, sp, #32
    stp     x29, x30, [sp, #16]
    add     x29, sp, #16
    mov     w1, w0
    add     w0, w1, #1
    ldp     x29, x30, [sp, #16]
    add     sp, sp, #32
    ret
    .size   recam_test_bti, .-recam_test_bti

// ---------------------------------------------------------------- PAC prologue
    .globl  recam_test_pac
    .type   recam_test_pac, %function
    .balign 16
recam_test_pac:
    paciasp
    sub     sp, sp, #32
    stp     x29, x30, [sp, #16]
    add     x29, sp, #16
    mov     w1, w0
    add     w0, w1, #1
    ldp     x29, x30, [sp, #16]
    add     sp, sp, #32
    autiasp
    ret
    .size   recam_test_pac, .-recam_test_pac

// ---------------------------------------------------------------- PC-relative
    .globl  recam_test_adrp
    .type   recam_test_adrp, %function
    .balign 16
recam_test_adrp:
    sub     sp, sp, #32
    // A local label; a global symbol would go through the GOT and emit no ADRP.
    adrp    x1, .Lrecam_adrp_data
    add     x1, x1, :lo12:.Lrecam_adrp_data
    mov     w1, w0
    add     w0, w1, #1
    add     sp, sp, #32
    ret
    .balign 8
.Lrecam_adrp_data:
    .quad   0
    .size   recam_test_adrp, .-recam_test_adrp

// ---------------------------------------------------------------- literal load
    .globl  recam_test_ldrlit
    .type   recam_test_ldrlit, %function
    .balign 16
recam_test_ldrlit:
    sub     sp, sp, #32
    nop
    ldr     x1, .Lrecam_ldrlit_data
    mov     w1, w0
    add     w0, w1, #1
    add     sp, sp, #32
    ret
    .balign 8
.Lrecam_ldrlit_data:
    .quad   0x1122334455667788
    .size   recam_test_ldrlit, .-recam_test_ldrlit

    .section .note.GNU-stack,"",%progbits
