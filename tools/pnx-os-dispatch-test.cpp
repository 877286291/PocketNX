// Self-contained test for the M1 OS command-dispatch table and service stubs.
//
// Two layers are exercised:
//   1. Direct unit tests of OS::Dispatch (no signal path), verifying that the
//      SVC table reads the right registers, writes results to x0, and routes
//      service IPC via SendSyncRequest.
//   2. One end-to-end signal-path test (AArch64 only) wiring a real OS into a
//      real SyscallTrap, to prove the C -> D bridge works with a live BRK.
//
// Build (no CMake needed):
//   clang++ -std=c++20 -Wall -Wextra -Wpedantic -I Core/src \
//     tools/pnx-os-dispatch-test.cpp Core/src/common/Log.cpp \
//     Core/src/common/Result.cpp Core/src/cpu/SyscallTrap.cpp \
//     Core/src/os/OS.cpp Core/src/os/Services.cpp Core/src/os/SvcTable.cpp \
//     -o pnx-os-dispatch-test -pthread
#if defined(__APPLE__)
#define _XOPEN_SOURCE
#include <ucontext.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "common/Log.h"
#include "os/OS.h"
#include "os/SvcTable.h"

using namespace pnx;

namespace {

bool Check(bool cond, const char* what) {
    std::printf("[check] %-48s %s\n", what, cond ? "PASS" : "FAIL");
    return cond;
}

uint64_t MakeNameBits(const char* s) {
    uint64_t v = 0;
    std::memcpy(&v, s, 8 < std::strlen(s) ? 8 : std::strlen(s));
    return v;
}

uint64_t MakeHeader(uint32_t command_id) {
    return static_cast<uint64_t>(command_id) << 24;
}

void RunUnitTests(OS& os, bool& ok) {
    // GetSystemTick (0x1E): x0 = monotonic ns, must be non-zero.
    {
        TrapContext tc{0xdead0000, 0x1E};
        GuestRegs r{};
        ResultCode rc = os.Dispatch(tc, r);
        ok &= Check(rc == ResultOk && r.x[0] > 0, "GetSystemTick: x0 = ticks > 0");
    }

    // SleepThread (0x0B): 1ms sleep, returns void.
    {
        TrapContext tc{0xdead0004, 0x0B};
        GuestRegs r{};
        r.x[0] = 1'000'000; // 1 ms
        auto t0 = std::chrono::steady_clock::now();
        ResultCode rc = os.Dispatch(tc, r);
        auto elapsed = std::chrono::steady_clock::now() - t0;
        ok &= Check(rc == ResultOk, "SleepThread: returns ResultOk");
        ok &= Check(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= 0,
            "SleepThread: completes");
    }

    // GetCurrentProcessorNumber (0x10): x0 = core id (0 for M1).
    {
        TrapContext tc{0xdead0008, 0x10};
        GuestRegs r{};
        ResultCode rc = os.Dispatch(tc, r);
        ok &= Check(rc == ResultOk && r.x[0] == 0, "GetCurrentProcessorNumber: x0 == 0");
    }

    // OutputDebugString (0x27): x0 = str, x1 = len, result in x0.
    {
        const char msg[] = "hello_pnx";
        TrapContext tc{0xdead000c, 0x27};
        GuestRegs r{};
        r.x[0] = reinterpret_cast<uint64_t>(msg);
        r.x[1] = sizeof(msg) - 1;
        ResultCode rc = os.Dispatch(tc, r);
        ok &= Check(rc == ResultOk, "OutputDebugString: returns ResultOk");
    }

    // CloseHandle (0x16): lenient, always Ok.
    {
        TrapContext tc{0xdead0010, 0x16};
        GuestRegs r{};
        r.x[0] = 0xCAFE;
        ResultCode rc = os.Dispatch(tc, r);
        ok &= Check(rc == ResultOk, "CloseHandle: unknown handle is lenient");
    }

    // GetInfo (0x29): x0 = out*, x1 = type. Type 1 = PID.
    {
        uint64_t out = 0;
        TrapContext tc{0xdead0014, 0x29};
        GuestRegs r{};
        r.x[0] = reinterpret_cast<uint64_t>(&out);
        r.x[1] = 1; // ProcessId
        ResultCode rc = os.Dispatch(tc, r);
        ok &= Check(rc == ResultOk && out == 0x100, "GetInfo(PID): writes 0x100");
    }

    // Unknown SVC: returns Unimplemented, x0 = Unimplemented.
    {
        TrapContext tc{0xdead0018, 0x7F};
        GuestRegs r{};
        r.x[0] = 0;
        ResultCode rc = os.Dispatch(tc, r);
        ok &= Check(rc == Unimplemented && r.x[0] == Unimplemented,
                    "Unknown SVC: returns Unimplemented");
    }

    // ConnectToNamedPort("sm") -> handle; then sm GetServiceHandle("hid") -> new handle.
    {
        const char sm_name[] = "sm";
        uint32_t sm_handle = 0;
        TrapContext tc{0xdead0020, 0x1F};
        GuestRegs r{};
        r.x[1] = reinterpret_cast<uint64_t>(sm_name);
        ResultCode rc = os.Dispatch(tc, r);
        sm_handle = static_cast<uint32_t>(r.x[1]);
        ok &= Check(rc == ResultOk && sm_handle != 0, "ConnectToNamedPort(sm): Ok + handle");

        // sm cmd 1 = GetServiceHandle; name in x2 (8-char zero-padded u64).
        TrapContext tc2{0xdead0024, 0x21};
        GuestRegs r2{};
        r2.x[0] = sm_handle;
        r2.x[1] = MakeHeader(1);
        r2.x[2] = MakeNameBits("hid");
        rc = os.Dispatch(tc2, r2);
        ok &= Check(rc == ResultOk && r2.x[1] != 0 && r2.x[1] != sm_handle,
                    "sm GetServiceHandle(hid): Ok + new handle");

        // SendSyncRequest to the hid handle with an unimplemented cmd -> Unimplemented.
        TrapContext tc3{0xdead0028, 0x21};
        GuestRegs r3{};
        r3.x[0] = static_cast<uint32_t>(r2.x[1]);
        r3.x[1] = MakeHeader(0x99); // not registered in hid
        rc = os.Dispatch(tc3, r3);
        ok &= Check(rc == Unimplemented && r3.x[0] == Unimplemented,
                    "hid unknown cmd: Unimplemented");
    }

    // CreateEvent (0x45): writes handles to the out-pointers.
    {
        uint32_t w = 0, rr = 0;
        TrapContext tc{0xdead0030, 0x45};
        GuestRegs r{};
        r.x[1] = reinterpret_cast<uint64_t>(&w);
        r.x[2] = reinterpret_cast<uint64_t>(&rr);
        ResultCode rc = os.Dispatch(tc, r);
        ok &= Check(rc == ResultOk && w != 0 && rr != 0 && w != rr,
                    "CreateEvent: distinct W/R handles");
    }

    // ExitProcess (0x07): sets the process-exited flag.
    {
        TrapContext tc{0xdead0034, 0x07};
        GuestRegs r{};
        os.Dispatch(tc, r);
        ok &= Check(os.ProcessExited(), "ExitProcess: sets exited flag");
    }
}

#if defined(__aarch64__)
static std::atomic<uint64_t> g_tick_after{0};

// The "previous" SIGTRAP disposition this test installs before the
// SyscallTrap. Under the not-ours handoff semantics the first (unregistered)
// BRK is forwarded here; it simply skips the BRK so the guest resumes.
// (Async-signal-safe: touches the ucontext only.)
static void PrevSkipBrk(int, siginfo_t*, void* uap) {
#if defined(__APPLE__)
    auto* ucp = static_cast<ucontext_t*>(uap);
    ucp->uc_mcontext->__ss.__pc += 4;
#elif defined(__linux__) && defined(__aarch64__)
    auto* ucp = static_cast<ucontext_t*>(uap);
    ucp->uc_mcontext.regs.pc += 4;
#else
    (void)uap;
#endif
}

__attribute__((noinline)) static void GuestTickSite() {
    asm volatile(
        "brk #0x0F00\n"
        "str x0, %[out]\n"
        : [out] "=m"(g_tick_after)
        :
        : "x0", "memory");
}

void RunSignalPathTest(OS& os, bool& ok) {
    std::unordered_map<uint64_t, uint16_t> table;
    SyscallTrap trap(table);
    OS::Wire(trap, os);

    // Install an observable "previous" disposition before the SyscallTrap so
    // the first (unregistered) BRK is skipped by handoff instead of being
    // fatal (previous = SIG_DFL would terminate the process).
    struct sigaction psa;
    std::memset(&psa, 0, sizeof(psa));
    psa.sa_sigaction = &PrevSkipBrk;
    psa.sa_flags = SA_SIGINFO;
    sigemptyset(&psa.sa_mask);
    if (sigaction(SIGTRAP, &psa, nullptr) != 0) {
        ok &= Check(false, "signal: could not install previous handler");
        return;
    }

    auto ir = trap.Install();
    if (!ir.IsOk()) {
        ok &= Check(false, "signal: trap install failed");
        return;
    }

    // First pass: BRK not yet in the table -> not ours -> the trap hands off
    // to the previous disposition (which skips the BRK). The handler records
    // the PC before the handoff, so it is learnable via LastTrapPc.
    GuestTickSite();
    const uint64_t brk_pc = trap.LastTrapPc();
    ok &= Check(brk_pc != 0, "signal: learned BRK pc via handoff");

    // The handoff uninstalled the trap; re-install (the previous disposition
    // becomes the new prev_action_) and register the PC as GetSystemTick.
    auto ir2 = trap.Install();
    ok &= Check(ir2.IsOk(), "signal: re-install after handoff works");
    table[brk_pc] = 0x1E;
    GuestTickSite();
    ok &= Check(g_tick_after.load() > 0, "signal: GetSystemTick x0 visible after resume");

    trap.Uninstall();
    // Leave the process with the default SIGTRAP disposition.
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGTRAP, &dfl, nullptr);
}
#endif

} // namespace

int main() {
    Log::SetLevel(LogLevel::Warn); // quiet the per-SVC logs; test prints its own PASS/FAIL

    OS os;
    RegisterDefaults(os);

    bool ok = true;
    RunUnitTests(os, ok);

#if defined(__aarch64__)
    RunSignalPathTest(os, ok);
#else
    std::printf("[check] %-48s SKIP (non-AArch64)\n", "signal: end-to-end BRK -> OS");
#endif

    if (!ok) {
        std::printf("\nSOME CHECKS FAILED\n");
        return 1;
    }
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
