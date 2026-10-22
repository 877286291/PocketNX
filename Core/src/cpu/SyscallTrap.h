#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <signal.h>
#include <unordered_map>

#include "common/Result.h"

namespace pnx {

// Snapshot of a trapped guest instruction (a BRK rewritten from SVC by the
// ProgramRewriter). Phase D extends this with the guest register file so OS
// service stubs can read arguments (x0..x7) and write the result back (x0).
struct TrapContext {
    uint64_t pc = 0;         // absolute address of the BRK instruction
    uint16_t svc_number = 0; // original Switch service-call number
};

// Full AArch64 guest register snapshot captured at the trap.
// x[0..30] are x0..x30 (x[30] == LR); sp is the stack pointer.
// The dispatch callback mutates this (typically writing the return value into
// x[0]); all modified registers are written back into the ucontext on resume.
// This is the D bridge: OS service stubs read arguments from x[0..7] and write
// results to x[0].
struct GuestRegs {
    uint64_t x[31]{};  // x0..x30
    uint64_t sp = 0;   // stack pointer
};

// Phase C: SIGTRAP-based trap handling for direct execution.
//
// Direct-execution flow:
//   guest code runs natively -> hits BRK (rewritten SVC) -> kernel raises
//   SIGTRAP -> SignalTrampoline -> HandleTrap:
//     1. read the faulting PC from the ucontext;
//     2. look the PC up in the pc->svc side table (built by ProgramRewriter);
//     3. invoke the dispatch callback (the OS service layer, Phase D);
//     4. advance PC past the 4-byte BRK so the guest resumes after the call.
//
// BRKs whose PC is NOT in the table are not PocketNX traps (e.g. a stray host
// BRK or a debugger breakpoint — SIGTRAP is process-wide). The handler
// restores the previous SIGTRAP disposition and hands the same ucontext to it,
// so an unhandled BRK behaves as if PocketNX had never installed a handler.
// The handoff uninstalls us; callers that want to keep trapping must
// Install() again. M1 scope: single active instance, single guest thread;
// dispatch work runs in signal context, so Phase D must keep it light or defer
// heavy work.
class SyscallTrap {
public:
    // Dispatch callback (the OS service layer). `regs` is a live snapshot of the
    // guest register file: read arguments from regs.x[0..7], write the result to
    // regs.x[0] (and any other register you must modify). The handler writes all
    // registers back before resuming the guest past the BRK.
    using DispatchFn = std::function<void(const TrapContext&, GuestRegs&)>;

    // `pc_to_svc` maps the absolute address of each rewritten BRK to the
    // original Switch svc number. Must outlive this object (pointer is stored,
    // the map is not copied).
    explicit SyscallTrap(const std::unordered_map<uint64_t, uint16_t>& pc_to_svc);
    ~SyscallTrap();

    SyscallTrap(const SyscallTrap&) = delete;
    SyscallTrap& operator=(const SyscallTrap&) = delete;

    // Install the SIGTRAP handler (saves the previous disposition).
    Result<void> Install();

    // Restore the previous SIGTRAP disposition. Also called by the destructor.
    void Uninstall();

    // Set the dispatch callback (the OS service layer).
    void SetDispatch(DispatchFn fn) { dispatch_ = std::move(fn); }

    // Tell the handler where the guest's TLS block lives (set by
    // Runtime::Prepare right after AllocateGuestContext). During dispatch the
    // handler swaps TPIDR_EL0 to the host TLS (stashed by the trampoline in
    // guest TLS slot 1, offset 8) so host C++ (errno, etc.) uses the real
    // host TLS. The swap only fires when the trapped thread's TPIDR_EL0
    // actually equals this block — a per-trap comparison, because SIGTRAP is
    // process-wide and a stray host BRK on another thread must not have its
    // TLS touched.
    void SetGuestTlsBlock(uint64_t tls_block) { guest_tls_block_ = tls_block; }

    // PC of the most recent trap handled (diagnostics; also how the smoke
    // test learns where its brk instruction lives). 0 if none yet.
    uint64_t LastTrapPc() const { return last_trap_pc_.load(std::memory_order_acquire); }

    // True while a trap is being processed (re-entrancy guard).
    bool InTrap() const { return in_trap_.load(std::memory_order_acquire); }

private:
    static void SignalTrampoline(int sig, siginfo_t* info, void* uap);
    void HandleTrap(int sig, siginfo_t* info, void* uap);

    static SyscallTrap* active_;  // single active instance (M1)

    const std::unordered_map<uint64_t, uint16_t>* table_;
    DispatchFn dispatch_;
    std::atomic<uint64_t> last_trap_pc_{0};
    std::atomic<bool> in_trap_{false};
    bool installed_ = false;
    uint64_t guest_tls_block_ = 0;
    struct sigaction prev_action_{};
};

} // namespace pnx
