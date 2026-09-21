// x86-64 hook targets for the payload self-test. Mirrors selftest_targets.S.

    .intel_syntax noprefix
    .text

// ---------------------------------------------------------------- plain prologue
    .globl  recam_test_plain
    .type   recam_test_plain, @function
    .balign 16
recam_test_plain:
    push    rbp                 // 55
    push    rbx                 // 53
    push    r12                 // 41 54
    push    r13                 // 41 55
    push    r14                 // 41 56
    push    r15                 // 41 57
    sub     rsp, 0x20           // 48 83 ec 20
    mov     eax, edi
    add     eax, 1
    add     rsp, 0x20
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret
    .size   recam_test_plain, .-recam_test_plain

// ---------------------------------------------------------------- endbr64
    .globl  recam_test_bti
    .type   recam_test_bti, @function
    .balign 16
recam_test_bti:
    endbr64                     // f3 0f 1e fa
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 0x20
    mov     eax, edi
    add     eax, 1
    add     rsp, 0x20
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret
    .size   recam_test_bti, .-recam_test_bti

// No PAC on x86-64; alias it to the plain case so the shared test list still links.
    .globl  recam_test_pac
    .set    recam_test_pac, recam_test_plain

// ---------------------------------------------------------------- RIP-relative
    .globl  recam_test_adrp
    .type   recam_test_adrp, @function
    .balign 16
recam_test_adrp:
    push    rbp
    lea     rax, [rip + recam_test_adrp_data]   // 48 8d 05 xx xx xx xx
    mov     eax, edi
    add     eax, 1
    pop     rbp
    ret
    .balign 8
recam_test_adrp_data:
    .quad   0
    .size   recam_test_adrp, .-recam_test_adrp

// A second unrelocatable form: a call, which is also PC-relative.
    .globl  recam_test_ldrlit
    .type   recam_test_ldrlit, @function
    .balign 16
recam_test_ldrlit:
    push    rbp
    push    rbx
    call    recam_test_ldrlit_next              // e8 xx xx xx xx
recam_test_ldrlit_next:
    pop     rax
    mov     eax, edi
    add     eax, 1
    pop     rbx
    pop     rbp
    ret
    .size   recam_test_ldrlit, .-recam_test_ldrlit

    .section .note.GNU-stack,"",@progbits
