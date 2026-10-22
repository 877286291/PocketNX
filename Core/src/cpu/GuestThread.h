#pragma once

#include <cstddef>
#include <cstdint>

#include "common/Result.h"

namespace pnx {

// One page-aligned guest stack. `base` is the low address, `top` the high
// address (the initial SP). Direct execution runs on the same ISA, so the
// guest stack is just a host allocation the guest thread's SP points into.
struct GuestStack {
    uint8_t* base = nullptr;
    uint8_t* top = nullptr;
    size_t size = 0;
};

// Execution context for one guest "process" under direct execution.
// On AArch64 the guest runs on the host CPU: we give it a stack, a TLS block
// (target of TPIDR_EL0), and an entry point. The SyscallTrap (Phase B/C) is
// installed before the jump; SVC instructions have been rewritten to BRK.
struct GuestContext {
    GuestStack stack;
    uint8_t* tls = nullptr;  // guest TLS block (TPIDR_EL0 target)
    size_t tls_size = 0;
    uint64_t entry = 0;      // absolute host address of the guest entry point
    uint64_t arg0 = 0;       // value placed in x0 at entry
};

// Allocate a guest stack + TLS block (page-aligned). `stack_size` and
// `tls_size` are rounded up to whole pages. Returns Ok on success.
Result<void> AllocateGuestContext(GuestContext& ctx, size_t stack_size, size_t tls_size);

// Release the host memory held by a context.
void FreeGuestContext(GuestContext& ctx);

// Set the guest registers (SP, TPIDR_EL0, x0) and branch to `entry`.
// Runs on the current thread; the guest executes with the given stack active.
// When the guest returns (br x30) we restore the host frame and return here.
//
// In the real runtime `entry` is the loaded NRO entry and the BRK trap handler
// is already installed (Phases B/C). For A4 we call it with a well-behaved
// probe function to prove the register setup is correct.
//
// AArch64-only (direct execution requires an arm64 host). On other hosts this
// is a no-op stub that returns Err(Unsupported).
Result<void> JumpToGuest(const GuestContext& ctx);

} // namespace pnx
