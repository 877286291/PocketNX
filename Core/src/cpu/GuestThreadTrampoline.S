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
	//
	// The guest is native code and may clobber ANY register — including the
	// callee-saved x19-x28, x29, x30, the platform register x18 and SP — so
	// the full host preserved set is saved in a 128-byte frame. x18 is
	// platform-reserved (Linux: VDSO pointer; Apple: reserved for the OS), so
	// a guest that uses it as a general register (legal on the AArch64 base
	// ISA) would destroy host platform state; save/restore it like any other
	// preserved register.
	//
	// Frame layout (all offsets from the frame base = SP after the first stp):
	//   [sp, #0]   x29, x30
	//   [sp, #16]  x18, x19
	//   [sp, #32]  x20, x21
	//   [sp, #48]  x22, x23
	//   [sp, #64]  x24, x25
	//   [sp, #80]  x26, x27
	//   [sp, #96]  x28
	//
	// The host frame SP itself cannot be held in a register (the guest may
	// clobber it), so it is stashed in slot 0 of the guest TLS block
	// ([tls, #0]); no guest register can reach it. TPIDR_EL0 points at that
	// block for the whole guest lifetime, so it is a valid indirection at
	// return time.
	//
	// Guest TLS reservation: slots 0 and 1 (bytes 0..15) are owned by the
	// trampoline — slot 0 = host frame SP, slot 1 = host TPIDR_EL0 (saved so
	// the SyscallTrap handler can run host C++ on the host TLS instead of
	// aliasing the guest block; on Linux libc errno lives at host-TLS+8,
	// which is exactly slot 1 here). A guest that uses TPIDR_EL0 for its
	// own TLS must start at slot 2 (offset 16).
	stp x29, x30, [sp, #-128]!
	stp x18, x19, [sp, #16]
	stp x20, x21, [sp, #32]
	stp x22, x23, [sp, #48]
	stp x24, x25, [sp, #64]
	stp x26, x27, [sp, #80]
	str x28, [sp, #96]
	mov x19, x0          // entry
	mov x20, x1          // arg0
	mov x21, x2          // tls
	mov x22, x3          // guest stack top
	mrs x9, tpidr_el0    // x9 = host TLS pointer
	str x9, [x21, #8]    // host TPIDR_EL0 -> guest TLS slot 1 (reserved)
	msr tpidr_el0, x21   // set guest thread pointer
	isb                  // TPIDR_EL0 update must be visible to guest TLS uses
	mov x9, sp           // (STR/LDR immediate cannot use SP as data register)
	str x9, [x21, #0]    // host frame SP -> guest TLS slot 0 (reserved)
	mov sp, x22          // switch to guest stack
	mov x0, x20          // first guest argument
	adr x30, .Lguest_ret // guest return address
	br x19               // enter guest code

.Lguest_ret:
	mrs x19, tpidr_el0   // guest TLS block (guest never touches TPIDR_EL0)
	ldr x9, [x19, #0]    // recover host frame SP
	mov sp, x9
	ldp x26, x27, [sp, #80]
	ldr x28, [sp, #96]
	ldp x24, x25, [sp, #64]
	ldp x22, x23, [sp, #48]
	ldp x20, x21, [sp, #32]
	ldp x18, x19, [sp, #16]
	ldp x29, x30, [sp], #128
	ret
#endif
