// Guest site C for pnx-syscall-trap-test: sets x0..x28 to distinct patterns,
// hits BRK #0x0F00, then stores all 29 registers to the caller's buffer.
//
// A standalone .S function (instead of inline asm in the .cpp) so the host
// compiler cannot allocate temporaries in x0..x28 around the BRK: 29 explicit
// register clobbers would exhaust the allocatable register set. x29/x30 and
// SP are never touched; callee-saved x19..x28 are saved/restored around the
// pattern window so the C caller's AAPCS state is fully preserved.
//
//   x0 in : uint64_t* out  (29 elements)
//
// At the BRK the register file holds x[i] = 0x1000 + i for i = 0..28. The
// SyscallTrap dispatch (test pass 5) verifies the capture and writes back
// x[i] = 0x2000 + i; the stores after the BRK then publish that result.
//
// The out pointer survives the pattern window on the stack (every GP register
// is overwritten by the pattern sequence). x9's own pattern value is parked
// on the stack just before the out pointer is reloaded, since the reload
// clobbers x9.
#if defined(__aarch64__)
	.text
	.p2align 2

#if defined(__APPLE__)
	.globl _pnx_trap_guest_full
_pnx_trap_guest_full:
#else
	.globl pnx_trap_guest_full
pnx_trap_guest_full:
#endif
	stp x29, x30, [sp, #-128]!
	str x0, [sp, #16]        // out pointer
	stp x19, x20, [sp, #32]
	stp x21, x22, [sp, #48]
	stp x23, x24, [sp, #64]
	stp x25, x26, [sp, #80]
	stp x27, x28, [sp, #96]

	mov x0, #0x1000
	mov x1, #0x1001
	mov x2, #0x1002
	mov x3, #0x1003
	mov x4, #0x1004
	mov x5, #0x1005
	mov x6, #0x1006
	mov x7, #0x1007
	mov x8, #0x1008
	mov x9, #0x1009
	mov x10, #0x100A
	mov x11, #0x100B
	mov x12, #0x100C
	mov x13, #0x100D
	mov x14, #0x100E
	mov x15, #0x100F
	mov x16, #0x1010
	mov x17, #0x1011
	mov x18, #0x1012
	mov x19, #0x1013
	mov x20, #0x1014
	mov x21, #0x1015
	mov x22, #0x1016
	mov x23, #0x1017
	mov x24, #0x1018
	mov x25, #0x1019
	mov x26, #0x101A
	mov x27, #0x101B
	mov x28, #0x101C
	brk #0x0F00

	str x9, [sp, #24]        // park x9's value (reloading the out ptr clobbers it)
	ldr x9, [sp, #16]        // x9 = out pointer
	str x0, [x9, #0]
	str x1, [x9, #8]
	str x2, [x9, #16]
	str x3, [x9, #24]
	str x4, [x9, #32]
	str x5, [x9, #40]
	str x6, [x9, #48]
	str x7, [x9, #56]
	str x8, [x9, #64]
	str x10, [x9, #80]
	str x11, [x9, #88]
	str x12, [x9, #96]
	str x13, [x9, #104]
	str x14, [x9, #112]
	str x15, [x9, #120]
	str x16, [x9, #128]
	str x17, [x9, #136]
	str x18, [x9, #144]
	str x19, [x9, #152]
	str x20, [x9, #160]
	str x21, [x9, #168]
	str x22, [x9, #176]
	str x23, [x9, #184]
	str x24, [x9, #192]
	str x25, [x9, #200]
	str x26, [x9, #208]
	str x27, [x9, #216]
	str x28, [x9, #224]
	ldr x0, [sp, #24]        // x0 = parked x9 value (x0 already stored)
	str x0, [x9, #72]        // out[9]

	ldp x27, x28, [sp, #96]
	ldp x25, x26, [sp, #80]
	ldp x23, x24, [sp, #64]
	ldp x21, x22, [sp, #48]
	ldp x19, x20, [sp, #32]
	ldp x29, x30, [sp], #128
	ret
#endif
