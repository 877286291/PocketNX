// Self-contained test for SyscallTrap (Phase C / D bridge).
//
// Exercises the real signal path end-to-end (no JIT / executable anon memory
// needed; brk instructions are compiled into this binary):
//
//   Pass 1: BRK not in the table -> unknown path, no dispatch, resumes; the
//           trap records the faulting PC.
//   Pass 2: register that PC as svc 0x0080, BRK again -> dispatch fires once
//           with the right svc + PC, resumes.
//   Pass 3: BRK again -> handler stays active (dispatch count grows).
//   Pass 4 (D bridge): a second guest site sets x0..x7 to known values, hits
//           its BRK; the handler reads the args (x0..x7), writes a "return
//           value" into x0; after resume the guest reads x0 back. Verifies the
//           full register capture -> dispatch -> writeback round-trip.
//
// AArch64 only. On other hosts the test prints a skip and exits 0 (CI green).
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

// Guest site A: a bare BRK, used for the install/lookup/repeat passes.
__attribute__((noinline)) static void GuestBrkSite() {
#if defined(__aarch64__)
    asm volatile ("brk #0x0F00");
#endif
}

// Guest site B: sets x0..x7 to known values, hits its BRK, then (after the
// handler resumes execution past the BRK) stores the post-trap x0 and x1 to
// globals. This proves the handler can read guest args and write a result.
static std::atomic<uint64_t> g_x0_after{0};
static std::atomic<uint64_t> g_x1_after{0};

__attribute__((noinline)) static void GuestSVCWithArgs() {
#if defined(__aarch64__)
    asm volatile (
        "mov x0, #0x11\n"
        "mov x1, #0x22\n"
        "mov x2, #0x33\n"
        "mov x3, #0x44\n"
        "mov x4, #0x55\n"
        "mov x5, #0x66\n"
        "mov x6, #0x77\n"
        "mov x7, #0x88\n"
        "brk #0x0F00\n"
        "str x0, %[out0]\n"
        "str x1, %[out1]\n"
        : [out0] "=m"(g_x0_after), [out1] "=m"(g_x1_after)
        :
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "memory"
    );
#endif
}

bool Check(bool cond, const char* what) {
    std::printf("[check] %-44s %s\n", what, cond ? "PASS" : "FAIL");
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
    std::atomic<uint64_t> site2_pc{0};        // learned when site B first traps
    std::atomic<uint64_t> seen_args0_7[8]{0}; // args the handler observed on site B

    trap.SetDispatch([&](const TrapContext& tc, GuestRegs& gregs) {
        dispatched.fetch_add(1, std::memory_order_relaxed);
        seen_svc.store(tc.svc_number, std::memory_order_relaxed);
        seen_pc.store(tc.pc, std::memory_order_relaxed);
        if (tc.pc == site2_pc.load(std::memory_order_relaxed) && site2_pc.load() != 0) {
            for (int i = 0; i < 8; ++i) {
                seen_args0_7[i].store(gregs.x[i], std::memory_order_relaxed);
            }
            gregs.x[0] = 0xABCD;  // "return value" the service writes back
        }
    });

    auto ir = trap.Install();
    if (!ir.IsOk()) {
        std::printf("Install failed\n");
        return 1;
    }

    bool ok = true;

    // --- Pass 1: BRK not in the table -> unknown path, no dispatch, resumes.
    GuestBrkSite();
    const uint64_t site1_pc = trap.LastTrapPc();
    ok &= Check(site1_pc != 0, "pass1: trap recorded a faulting PC");
    ok &= Check(dispatched.load() == 0, "pass1: no dispatch for unknown BRK");
    std::printf("[trap] pass1: unknown BRK resumed at pc=0x%" PRIx64 "\n", site1_pc);

    // --- Pass 2: register the PC, BRK again -> dispatch fires, resumes.
    table[site1_pc] = 0x0080;
    GuestBrkSite();
    ok &= Check(dispatched.load() == 1, "pass2: dispatch called exactly once");
    ok &= Check(seen_svc.load() == 0x0080, "pass2: dispatch saw svc 0x0080");
    ok &= Check(seen_pc.load() == site1_pc, "pass2: dispatch saw the right PC");
    std::printf("[trap] pass2: svc 0x%04x dispatched at pc=0x%" PRIx64 "\n",
                seen_svc.load(), seen_pc.load());

    // --- Pass 3: repeat -> handler stays active.
    GuestBrkSite();
    ok &= Check(dispatched.load() == 2, "pass3: handler still active (2 total)");

    // --- Pass 4: register capture / writeback round-trip on site B.
    // First, learn site B's PC (unknown path, no dispatch).
    GuestSVCWithArgs();
    const uint64_t siteB_pc = trap.LastTrapPc();
    site2_pc.store(siteB_pc, std::memory_order_release);
    ok &= Check(siteB_pc != 0 && siteB_pc != site1_pc, "pass4: site B has its own PC");

    // Now register it and run again; the handler should read x0..x7 and set x0.
    table[siteB_pc] = 0x0099;
    GuestSVCWithArgs();
    ok &= Check(dispatched.load() == 3, "pass4: site B dispatched (3 total)");

    // Verify the handler observed the guest args x0..x7 we set.
    const uint64_t expect_args[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    bool args_ok = true;
    for (int i = 0; i < 8; ++i) {
        args_ok &= (seen_args0_7[i].load() == expect_args[i]);
    }
    ok &= Check(args_ok, "pass4: handler read x0..x7 correctly");

    // Verify the handler's writeback to x0 took effect after resume.
    ok &= Check(g_x0_after.load() == 0xABCD, "pass4: handler x0 writeback visible (0xABCD)");
    ok &= Check(g_x1_after.load() == 0x22, "pass4: untouched x1 preserved (0x22)");

    trap.Uninstall();

    if (!ok) {
        std::printf("\nSOME CHECKS FAILED\n");
        return 1;
    }
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
