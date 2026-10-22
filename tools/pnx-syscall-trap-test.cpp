// Self-contained test for SyscallTrap (Phase C).
//
// Strategy (no JIT / executable anonymous memory needed):
//   1. Install the trap with an EMPTY pc->svc table.
//   2. Run a `brk` from host code (compiled into this binary) -> SIGTRAP ->
//      "unknown BRK" path resumes; the trap records the faulting PC.
//   3. Register that PC in the table as svc 0x0080.
//   4. Run the same `brk` again -> the table hits, the dispatch callback fires
//      with the svc number, execution resumes, and the function returns.
//
// This exercises the real signal path end-to-end: BRK -> SIGTRAP -> ucontext
// PC read -> table lookup -> dispatch -> PC advance -> resume.
//
// AArch64 only (brk / SIGTRAP specifics). On other hosts the test prints a
// skip and exits 0 so CI stays green.
//
// Build (no CMake needed):
//   clang++ -std=c++20 -Wall -Wextra -I Core/src tools/pnx-syscall-trap-test.cpp \
//     Core/src/common/Log.cpp Core/src/common/Result.cpp Core/src/cpu/SyscallTrap.cpp \
//     -o pnx-syscall-trap-test -pthread
#include <atomic>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <unordered_map>

#include "common/Log.h"
#include "cpu/SyscallTrap.h"

using namespace pnx;

namespace {

// A "guest site": a single BRK instruction, compiled into the host binary so
// triggering it needs no executable anonymous memory / JIT entitlement.
__attribute__((noinline)) static void GuestBrkSite() {
#if defined(__aarch64__)
    asm volatile ("brk #0x0F00");
#endif
}

bool Check(bool cond, const char* what) {
    std::printf("[check] %-40s %s\n", what, cond ? "PASS" : "FAIL");
    return cond;
}

} // namespace

int main() {
    Log::SetLevel(LogLevel::Info);

#if !defined(__aarch64__)
    std::printf("skipped: BRK/SIGTRAP trap test requires an AArch64 host\n");
    return 0;
#endif

    std::unordered_map<uint64_t, uint16_t> table;  // empty at first
    SyscallTrap trap(table);

    std::atomic<int> dispatched{0};
    std::atomic<uint16_t> seen_svc{0};
    std::atomic<uint64_t> seen_pc{0};
    trap.SetDispatch([&](const TrapContext& tc) {
        dispatched.fetch_add(1, std::memory_order_relaxed);
        seen_svc.store(tc.svc_number, std::memory_order_relaxed);
        seen_pc.store(tc.pc, std::memory_order_relaxed);
    });

    auto ir = trap.Install();
    if (!ir.IsOk()) {
        std::printf("Install failed\n");
        return 1;
    }

    bool ok = true;

    // --- Pass 1: BRK not in the table -> unknown path, no dispatch, resumes.
    GuestBrkSite();
    const uint64_t brk_pc = trap.LastTrapPc();
    ok &= Check(brk_pc != 0, "pass1: trap recorded a faulting PC");
    ok &= Check(dispatched.load() == 0, "pass1: no dispatch for unknown BRK");
    std::printf("[trap] pass1: unknown BRK resumed at pc=0x%" PRIx64 "\n", brk_pc);

    // --- Pass 2: register the PC, BRK again -> dispatch fires, resumes.
    table[brk_pc] = 0x0080;
    GuestBrkSite();
    ok &= Check(dispatched.load() == 1, "pass2: dispatch called exactly once");
    ok &= Check(seen_svc.load() == 0x0080, "pass2: dispatch saw svc 0x0080");
    ok &= Check(seen_pc.load() == brk_pc, "pass2: dispatch saw the right PC");
    std::printf("[trap] pass2: svc 0x%04x dispatched at pc=0x%" PRIx64 "\n",
                seen_svc.load(), seen_pc.load());

    // --- Pass 3: a third BRK must dispatch again (handler stays installed).
    GuestBrkSite();
    // pass1 dispatched 0 (unknown BRK), pass2 + pass3 dispatch 1 each.
    ok &= Check(dispatched.load() == 2, "pass3: handler still active (2 total)");

    trap.Uninstall();

    if (!ok) {
        std::printf("\nSOME CHECKS FAILED\n");
        return 1;
    }
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
