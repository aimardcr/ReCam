// Trampoline scratch in our own .text, so no SELinux execmem is needed.

    .text
    .globl  recam_tramp_arena
    .globl  recam_tramp_arena_end
    .balign 64
recam_tramp_arena:
    .space  4096, 0
recam_tramp_arena_end:

    .section .note.GNU-stack,"",%progbits
