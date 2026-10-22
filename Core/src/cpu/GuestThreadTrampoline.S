#if defined(__aarch64__)
    .text
    .p2align 2

#if defined(__APPLE__)
    .globl _pnx_guest_trampoline
_pnx_guest_trampoline:
#else
    .globl pnx_guest_trampoline
pnx_guest_trampoline:
#endif
    // AAPCS:
    // x0 = entry
    // x1 = arg0
    // x2 = tls
    // x3 = guest stack top
    stp x29, x30, [sp, -32]!
    stp x19, x20, [sp, -16]!
    mov x19, x0          // entry
    mov x20, x1          // arg0
    mov x21, x2          // tls
    mov x22, x3          // guest stack top
    mov x23, sp          // save host SP

    msr tpidr_el0, x21   // set guest thread pointer
    mov sp, x22          // switch to guest stack
    mov x0, x20          // first guest argument
    adr x30, .Lguest_ret // guest return address
    br x19               // enter guest code

.Lguest_ret:
    mov sp, x23
    ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #32
    ret
#endif
