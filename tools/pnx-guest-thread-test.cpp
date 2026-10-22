// Self-contained test for the A4 guest execution context (stack + TLS + register setup).
//
// Allocates a guest stack and TLS block, then branches into a host "probe"
// function through the direct-execution trampoline. The probe reads SP,
// TPIDR_EL0 and x0 from inside the guest context and reports them back. The
// test asserts the trampoline set all three correctly.
//
// The probe is a normal (well-behaved, AAPCS) function standing in for guest
// code; in the real runtime the entry is the loaded NRO with SVC->BRK rewriting
// and the trap handler installed (Phases B/C).
//
// Build (no CMake needed):
//   clang++ -std=c++20 -Wall -Wextra -I Core/src tools/pnx-guest-thread-test.cpp \
//     Core/src/common/Log.cpp Core/src/common/Result.cpp Core/src/cpu/GuestThread.cpp \
//     -o pnx-guest-thread-test -pthread
//
// Note: the trampoline only executes on an AArch64 host. On x86_64 the test
// still verifies allocation/teardown but skips the jump.
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdint>

#include "common/Log.h"
#include "cpu/GuestThread.h"

using namespace pnx;

namespace {

struct ProbeResult {
    uint64_t x0 = 0;
    uint64_t tls = 0;
    uint64_t sp = 0;
    uint64_t ret = 0;
};
static ProbeResult g_probe;

// The "guest": a well-behaved AAPCS function that reports the register state
// it sees when entered by the trampoline. x0 arrives as its first parameter
// (the trampoline places arg0 in x0 before branching here).
extern "C" uint64_t guest_probe(uint64_t x0_in) {
#if defined(__aarch64__)
    uint64_t tls = 0;
    uint64_t sp = 0;
    asm volatile ("mrs %0, tpidr_el0" : "=r"(tls));
    asm volatile ("mov %0, sp" : "=r"(sp));
    g_probe.x0 = x0_in;
    g_probe.tls = tls;
    g_probe.sp = sp;
    g_probe.ret = 42;
#endif
    return 42;
}

bool Check(bool cond, const char* what) {
    std::printf("[check] %-22s %s\n", what, cond ? "PASS" : "FAIL");
    return cond;
}

} // namespace

int main() {
    Log::SetLevel(LogLevel::Info);

    const size_t kStackSize = 64 * 1024;  // 64 KiB guest stack
    const size_t kTlsSize = 0x1000;        // one page of TLS

    GuestContext ctx;
    auto alloc = AllocateGuestContext(ctx, kStackSize, kTlsSize);
    if (!alloc.IsOk()) {
        std::printf("allocation failed\n");
        return 1;
    }
    assert(ctx.stack.base && ctx.stack.top && ctx.tls);
    assert(ctx.stack.top > ctx.stack.base);
    assert((reinterpret_cast<uint64_t>(ctx.stack.base) % 0x1000) == 0);
    assert((reinterpret_cast<uint64_t>(ctx.tls) % 0x1000) == 0);
    std::printf("[alloc] stack=[0x%" PRIX64 "..0x%" PRIX64 ") tls=0x%" PRIX64 "\n",
                (uint64_t)ctx.stack.base, (uint64_t)ctx.stack.base + ctx.stack.size,
                (uint64_t)ctx.tls);

#if defined(__aarch64__)
    ctx.entry = reinterpret_cast<uint64_t>(guest_probe);
    ctx.arg0 = 0xDEADBEEFCAFEBABEull;

    auto jump = JumpToGuest(ctx);
    assert(jump.IsOk() && "JumpToGuest should succeed on AArch64");

    bool ok = true;
    ok &= Check(g_probe.x0 == 0xDEADBEEFCAFEBABEull, "x0 == arg0");
    ok &= Check(g_probe.tls == reinterpret_cast<uint64_t>(ctx.tls),
                "TPIDR_EL0 == guest tls");
    // The probe's sp sits one frame below the initial top, but always inside the stack.
    ok &= Check(g_probe.sp >= reinterpret_cast<uint64_t>(ctx.stack.base) &&
                g_probe.sp <= reinterpret_cast<uint64_t>(ctx.stack.top),
                "SP inside guest stack");
    ok &= Check(g_probe.ret == 42, "guest return value");
    if (!ok) {
        std::printf("\nPROBE FAILED\n");
        return 1;
    }
#else
    std::printf("[skip]  trampoline requires AArch64 host; allocation verified only\n");
    auto jump = JumpToGuest(ctx);
    assert(!jump.IsOk() && "should report Unsupported on non-AArch64");
#endif

    FreeGuestContext(ctx);
    assert(!ctx.stack.base && !ctx.tls);
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
