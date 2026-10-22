// Self-contained test for SyscallTrap (Phase C / D bridge).
//
// Exercises the real signal path end-to-end (no JIT / executable anon memory
// needed; brk instructions are compiled into this binary):
//
//   A "previous" SIGTRAP disposition (an async-signal-safe handler that
//   records the PC, counts calls, and skips the BRK) is installed BEFORE the
//   SyscallTrap, so unknown-BRK handoffs land somewhere observable.
//
//   Pass 1 (handoff): BRK not in the table -> the handler must NOT bump the
//           PC itself, must restore the previous disposition, and must hand
//           the same ucontext to it. The previous handler observes exactly
//           one call at the BRK's PC; execution resumes after the BRK; the
//           trap is uninstalled by the handoff (re-Install works).
//   Pass 2: register that PC as svc 0x0080, BRK again -> dispatch fires once
//           with the right svc + PC, resumes.
//   Pass 3: BRK again -> handler stays active (dispatch count grows).
//   Pass 4 (D bridge): a second guest site sets x0..x7 to known values and
//           hits its BRK. First hit is unknown (handoff learns site B's PC
//           and must leave x0..x1 untouched); then it is registered and the
//           handler reads the args (x0..x7) and writes a "return value" into
//           x0; after resume the guest reads x0 back.
//   Pass 5 (full round-trip): a third guest site sets x0..x28 to distinct
//           patterns (0x1000+i) and hits its BRK; the handler verifies every
//           captured register and writes back distinct new values
//           (0x2000+i); after resume the guest stores all 29 registers.
//           This proves the complete capture -> dispatch -> writeback path,
//           including Apple's split __x[0..28]/__fp/__lr ucontext layout.
//           NOTE (macOS): the kernel resets the platform register x18 to 0 on
//           every signal return and ignores ucontext.__x[18] (probed; see
//           docs/ENVIRONMENT-ISSUES.md), so on Apple hosts the x18 writeback
//           is exempted; the x18 CAPTURE at trap entry is still verified.
//
// AArch64 only. On other hosts the test prints a skip and exits 0 (CI green).
//
// Build (no CMake needed):
//   clang++ -std=c++20 -Wall -Wextra -I Core/src tools/pnx-syscall-trap-test.cpp \
//     Core/src/common/Log.cpp Core/src/common/Result.cpp Core/src/cpu/SyscallTrap.cpp \
//     -o pnx-syscall-trap-test -pthread
#if defined(__APPLE__)
#define _XOPEN_SOURCE
#include <ucontext.h>
#endif

#include <unistd.h>
#include <atomic>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "common/Log.h"
#include "cpu/SyscallTrap.h"

using namespace pnx;

namespace {

// Crash diagnostics (SIGSEGV/SIGBUS/SIGILL only — never SIGTRAP, which the
// test drives itself). Prints the faulting state and exits.
void CrashHandler(int signo, siginfo_t* info, void* uap) {
#if defined(__APPLE__)
    auto* ucp = static_cast<ucontext_t*>(uap);
    const auto& ss = ucp->uc_mcontext->__ss;
    char buf[512];
    const int n = std::snprintf(buf, sizeof(buf),
                                "CRASH signo=%d code=%d pc=0x%llx sp=0x%llx "
                                "x0=0x%llx x9=0x%llx lr=0x%llx fault=0x%llx\n",
                                signo, (int)info->si_code,
                                (unsigned long long)ss.__pc,
                                (unsigned long long)ss.__sp,
                                (unsigned long long)ss.__x[0],
                                (unsigned long long)ss.__x[9],
                                (unsigned long long)ss.__lr,
                                (unsigned long long)reinterpret_cast<uintptr_t>(
                                    info->si_addr));
    (void)!::write(2, buf, static_cast<size_t>(n));
#endif
    std::exit(128 + signo);
}

// The "previous disposition" installed before the SyscallTrap. Unknown BRKs
// are handed off here. Must be async-signal-safe: atomics + ucontext only.
std::atomic<int> g_prev_calls{0};
std::atomic<uint64_t> g_prev_pc{0};

void PrevTrapHandler(int sig, siginfo_t* info, void* uap) {
    (void)sig;
    (void)info;
#if defined(__APPLE__)
    auto* ucp = static_cast<ucontext_t*>(uap);
    const uint64_t pc = ucp->uc_mcontext->__ss.__pc;
    g_prev_pc.store(pc, std::memory_order_relaxed);
    g_prev_calls.fetch_add(1, std::memory_order_relaxed);
    ucp->uc_mcontext->__ss.__pc = pc + 4; // skip the BRK so we can resume
#elif defined(__linux__) && defined(__aarch64__)
    auto* ucp = static_cast<ucontext_t*>(uap);
    const uint64_t pc = static_cast<uint64_t>(ucp->uc_mcontext.regs.pc);
    g_prev_pc.store(pc, std::memory_order_relaxed);
    g_prev_calls.fetch_add(1, std::memory_order_relaxed);
    ucp->uc_mcontext.regs.pc = pc + 4;
#else
    (void)uap;
#endif
}

// Guest site A: a bare BRK, used for the install/handoff/repeat passes.
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

// Guest site C: sets x0..x28 to distinct patterns, hits its BRK, then stores
// all 29 registers to g_full_regs. Implemented in pnx-syscall-trap-guest.S so
// the host compiler cannot allocate temporaries in x0..x28 around the BRK
// (29 explicit register clobbers would exhaust the allocatable register set).
// x29/x30/SP are deliberately untouched by the site.
static uint64_t g_full_regs[29]{};

extern "C" void pnx_trap_guest_full(uint64_t* out);

__attribute__((noinline)) static void GuestFullRegs() {
#if defined(__aarch64__)
    pnx_trap_guest_full(g_full_regs);
#endif
}

bool Check(bool cond, const char* what) {
    std::printf("[check] %-48s %s\n", what, cond ? "PASS" : "FAIL");
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
    std::atomic<uint64_t> site2_pc{0};  // learned when site B first traps
    std::atomic<uint64_t> site3_pc{0};  // learned when site C first traps
    std::atomic<uint64_t> seen_args0_7[8]{0}; // args the handler observed on site B
    std::atomic<int> capture_bad{-1};  // index of first capture mismatch, -1 = OK

    trap.SetDispatch([&](const TrapContext& tc, GuestRegs& gregs) {
        dispatched.fetch_add(1, std::memory_order_relaxed);
        seen_svc.store(tc.svc_number, std::memory_order_relaxed);
        seen_pc.store(tc.pc, std::memory_order_relaxed);
        if (tc.pc == site2_pc.load(std::memory_order_relaxed) &&
            site2_pc.load(std::memory_order_relaxed) != 0) {
            for (int i = 0; i < 8; ++i) {
                seen_args0_7[i].store(gregs.x[i], std::memory_order_relaxed);
            }
            gregs.x[0] = 0xABCD;  // "return value" the service writes back
        }
        if (tc.pc == site3_pc.load(std::memory_order_relaxed) &&
            site3_pc.load(std::memory_order_relaxed) != 0) {
            // Verify the full capture, then write back distinct new values.
            for (int i = 0; i < 29; ++i) {
                if (gregs.x[i] != static_cast<uint64_t>(0x1000 + i)) {
                    capture_bad.store(i, std::memory_order_relaxed);
                    return;
                }
            }
            for (int i = 0; i < 29; ++i) {
                gregs.x[i] = static_cast<uint64_t>(0x2000 + i);
            }
        }
    });

    {
        struct sigaction ch;
        std::memset(&ch, 0, sizeof(ch));
        ch.sa_sigaction = &CrashHandler;
        ch.sa_flags = SA_SIGINFO;
        sigemptyset(&ch.sa_mask);
        sigaction(SIGSEGV, &ch, nullptr);
        sigaction(SIGBUS, &ch, nullptr);
        sigaction(SIGILL, &ch, nullptr);
    }

    // Install the observable "previous" disposition BEFORE the SyscallTrap so
    // unknown-BRK handoffs are visible and resumable.
    struct sigaction psa;
    std::memset(&psa, 0, sizeof(psa));
    psa.sa_sigaction = &PrevTrapHandler;
    psa.sa_flags = SA_SIGINFO;
    sigemptyset(&psa.sa_mask);
    if (sigaction(SIGTRAP, &psa, nullptr) != 0) {
        std::printf("failed to install previous SIGTRAP disposition\n");
        return 1;
    }

    auto ir = trap.Install();
    if (!ir.IsOk()) {
        std::printf("Install failed\n");
        return 1;
    }

    bool ok = true;

    // --- Pass 1: unknown BRK -> handoff to the previous disposition.
    GuestBrkSite();
    const uint64_t site1_pc = g_prev_pc.load(std::memory_order_acquire);
    ok &= Check(site1_pc != 0, "pass1: previous handler saw the trap");
    ok &= Check(g_prev_calls.load() == 1, "pass1: handoff happened exactly once");
    ok &= Check(dispatched.load() == 0, "pass1: no dispatch for unknown BRK");
    ok &= Check(!trap.InTrap(), "pass1: re-entrancy flag cleared after handoff");
    std::printf("[trap] pass1: unknown BRK handed off at pc=0x%" PRIx64 "\n", site1_pc);

    // The handoff uninstalled us; re-install for the remaining passes.
    auto ir2 = trap.Install();
    ok &= Check(ir2.IsOk(), "pass1: re-install after handoff works");

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
    // First hit is unknown: the handoff learns site B's PC and must leave the
    // guest's x0..x1 untouched (the ucontext is not modified by us).
    GuestSVCWithArgs();
    const uint64_t siteB_pc = g_prev_pc.load(std::memory_order_acquire);
    ok &= Check(g_prev_calls.load() == 2, "pass4: handoff observed site B once");
    ok &= Check(siteB_pc != 0 && siteB_pc != site1_pc, "pass4: site B has its own PC");
    ok &= Check(g_x0_after.load() == 0x11, "pass4: handoff left x0 untouched (0x11)");
    ok &= Check(g_x1_after.load() == 0x22, "pass4: handoff left x1 untouched (0x22)");
    site2_pc.store(siteB_pc, std::memory_order_release);

    auto ir3 = trap.Install();
    ok &= Check(ir3.IsOk(), "pass4: re-install after handoff works");
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

    // --- Pass 5: full x0..x28 capture -> writeback round-trip.
    // First hit is unknown: the handoff learns site C's PC.
    GuestFullRegs();
    const uint64_t siteC_pc = g_prev_pc.load(std::memory_order_acquire);
    ok &= Check(g_prev_calls.load() == 3, "pass5: handoff observed site C once");
    ok &= Check(siteC_pc != 0 && siteC_pc != siteB_pc, "pass5: site C has its own PC");
    site3_pc.store(siteC_pc, std::memory_order_release);

    auto ir4 = trap.Install();
    ok &= Check(ir4.IsOk(), "pass5: re-install after handoff works");
    table[siteC_pc] = 0x00AA;
    GuestFullRegs();
    ok &= Check(dispatched.load() == 4, "pass5: site C dispatched (4 total)");

    ok &= Check(capture_bad.load() == -1, "pass5: all x0..x28 captured correctly");
    if (capture_bad.load() != -1) {
        std::printf("[trap] pass5: first capture mismatch at x%d\n", capture_bad.load());
    }

    // On macOS the kernel resets x18 to 0 on signal return and ignores the
    // ucontext value we wrote (probed empirically — see
    // docs/ENVIRONMENT-ISSUES.md). The handler still writes x18 = 0x2012 into
    // the ucontext; only the guest-visible result is the kernel's 0. Exempt
    // x18 from the writeback check on Apple; capture is still verified.
    int full_bad = 0;
    for (int i = 0; i < 29; ++i) {
#if defined(__APPLE__)
        if (i == 18) continue;
#endif
        if (g_full_regs[i] != static_cast<uint64_t>(0x2000 + i)) {
            ++full_bad;
        }
    }
    ok &= Check(full_bad == 0, "pass5: writeback values visible (0x2000+i; x18 exempted on macOS)");
    if (full_bad != 0) {
        for (int i = 0; i < 29; ++i) {
#if defined(__APPLE__)
            if (i == 18) continue;
#endif
            if (g_full_regs[i] != static_cast<uint64_t>(0x2000 + i)) {
                std::printf("[trap] pass5: x%d = 0x%" PRIx64 " (want 0x%" PRIx64 ")\n", i,
                            g_full_regs[i],
                            (unsigned long long)static_cast<uint64_t>(0x2000 + i));
            }
        }
    }

    trap.Uninstall();
    // Leave the process with a normal default SIGTRAP disposition.
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGTRAP, &dfl, nullptr);

    if (!ok) {
        std::printf("\nSOME CHECKS FAILED\n");
        return 1;
    }
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
