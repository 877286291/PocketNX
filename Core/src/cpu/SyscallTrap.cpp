#if defined(__APPLE__)
// ucontext_t (used to read the faulting PC in a SIGTRAP handler) is a
// deprecated POSIX facility on macOS; opt in explicitly so the type is declared.
#define _XOPEN_SOURCE
#endif

#include "cpu/SyscallTrap.h"

#include <cinttypes>
#include <cstring>

#include "common/Log.h"

#if defined(__APPLE__)
#include <ucontext.h>
#include <mach/mach.h>
#elif defined(__linux__)
#include <sys/ucontext.h>
#endif

namespace pnx {

namespace {

#if defined(__APPLE__)
inline uint64_t ReadPc(ucontext_t* ucp) {
    return ucp->uc_mcontext->__ss.__pc;
}
inline void BumpPc(ucontext_t* ucp) {
    ucp->uc_mcontext->__ss.__pc += 4;
}
#elif defined(__linux__) && defined(__x86_64__)
inline uint64_t ReadPc(ucontext_t* ucp) {
    return static_cast<uint64_t>(ucp->uc_mcontext.gregs[REG_RIP]);
}
inline void BumpPc(ucontext_t* ucp) {
    ucp->uc_mcontext.gregs[REG_RIP] += 4;
}
#elif defined(__linux__) && defined(__aarch64__)
inline uint64_t ReadPc(ucontext_t* ucp) {
    return static_cast<uint64_t>(ucp->uc_mcontext.regs.pc);
}
inline void BumpPc(ucontext_t* ucp) {
    ucp->uc_mcontext.regs.pc += 4;
}
#endif

// Capture the guest register file (x0..x30, sp) from the trap ucontext.
// Apple's arm_thread_state64_t splits the file: __x[0..28] = x0..x28,
// __fp = x29, __lr = x30, __sp = sp. There is no __x[29..31].
#if defined(__APPLE__)
inline void CaptureRegs(ucontext_t* ucp, GuestRegs& g) {
    auto& ss = ucp->uc_mcontext->__ss;
    for (int i = 0; i < 29; ++i) g.x[i] = ss.__x[i]; // x0..x28
    g.x[29] = ss.__fp;   // x29
    g.x[30] = ss.__lr;   // x30
    g.sp = ss.__sp;
}
inline void WriteBackRegs(ucontext_t* ucp, const GuestRegs& g) {
    auto& ss = ucp->uc_mcontext->__ss;
    for (int i = 0; i < 29; ++i) ss.__x[i] = g.x[i]; // x0..x28
    ss.__fp = g.x[29];   // x29
    ss.__lr = g.x[30];   // x30
    ss.__sp = g.sp;
}
#elif defined(__linux__) && defined(__aarch64__)
inline void CaptureRegs(ucontext_t* ucp, GuestRegs& g) {
    auto& r = ucp->uc_mcontext.regs;
    for (int i = 0; i < 31; ++i) g.x[i] = r.regs[i];
    g.sp = r.sp;
}
inline void WriteBackRegs(ucontext_t* ucp, const GuestRegs& g) {
    auto& r = ucp->uc_mcontext.regs;
    for (int i = 0; i < 31; ++i) r.regs[i] = g.x[i];
    r.sp = g.sp;
}
#else
// x86_64 host: an AArch64 guest cannot run natively, so register capture is a
// no-op (the guest registers are left zeroed). pc/svc still work for tests.
inline void CaptureRegs(ucontext_t*, GuestRegs&) {}
inline void WriteBackRegs(ucontext_t*, const GuestRegs&) {}
#endif

} // namespace

SyscallTrap* SyscallTrap::active_ = nullptr;

SyscallTrap::SyscallTrap(const std::unordered_map<uint64_t, uint16_t>& pc_to_svc)
    : table_(&pc_to_svc) {}

SyscallTrap::~SyscallTrap() {
    Uninstall();
}

Result<void> SyscallTrap::Install() {
    if (installed_) {
        return Result<void>::Ok();
    }
#if defined(__APPLE__) || defined(__linux__)
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = &SyscallTrap::SignalTrampoline;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO; // deliver siginfo_t + ucontext
    if (sigaction(SIGTRAP, &sa, &prev_action_) != 0) {
        PNX_LOG_ERROR("SyscallTrap", "sigaction(SIGTRAP) failed");
        return Result<void>::Err(InvalidArg);
    }
    active_ = this;
    installed_ = true;
    PNX_LOG_INFO("SyscallTrap", "SIGTRAP handler installed");
    return Result<void>::Ok();
#else
    PNX_LOG_WARN("SyscallTrap", "signal traps not supported on this platform");
    return Result<void>::Err(Unsupported);
#endif
}

void SyscallTrap::Uninstall() {
#if defined(__APPLE__) || defined(__linux__)
    if (installed_) {
        sigaction(SIGTRAP, &prev_action_, nullptr);
        installed_ = false;
        if (active_ == this) {
            active_ = nullptr;
        }
        PNX_LOG_INFO("SyscallTrap", "SIGTRAP handler uninstalled");
    }
#endif
}

void SyscallTrap::SignalTrampoline(int sig, siginfo_t* info, void* uap) {
    (void)sig;
    (void)info;
    auto* self = active_;
    if (!self || self->in_trap_) {
        // Not ours (or re-entrant) — leave it for the default disposition.
        return;
    }
    self->HandleTrap(uap);
}

void SyscallTrap::HandleTrap(void* uap) {
    in_trap_.store(true, std::memory_order_release);
#if defined(__APPLE__) || (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)))
    auto* ucp = static_cast<ucontext_t*>(uap);
    const uint64_t pc = ReadPc(ucp);
    last_trap_pc_.store(pc, std::memory_order_release);

    uint16_t svc = 0;
    bool ours = false;
    if (table_) {
        auto it = table_->find(pc);
        if (it != table_->end()) {
            svc = it->second;
            ours = true;
        }
    }

    if (ours) {
        TrapContext tc;
        tc.pc = pc;
        tc.svc_number = svc;
        GuestRegs gregs;
        CaptureRegs(ucp, gregs);
        PNX_LOG_INFO("SyscallTrap", "guest SVC trap at pc=0x%" PRIx64 " -> svc 0x%04x",
                     pc, svc);
        if (dispatch_) {
            dispatch_(tc, gregs);
            WriteBackRegs(ucp, gregs);  // persist any register the handler changed
        } else {
            PNX_LOG_WARN("SyscallTrap", "svc 0x%04x has no dispatch handler (Phase D not wired)",
                         svc);
        }
    } else {
        PNX_LOG_WARN("SyscallTrap", "BRK at 0x%" PRIx64 " is not a PocketNX trap; resuming", pc);
    }

    // Resume after the 4-byte BRK instruction.
    BumpPc(ucp);
#else
    (void)uap;
#endif
    in_trap_.store(false, std::memory_order_release);
}

} // namespace pnx
