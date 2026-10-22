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
// BRKs whose PC is NOT in the table are logged and execution resumes (a guest
// may use its own brk for debug purposes); this keeps the handler safe for the
// whole process. M1 scope: single active instance, single guest thread;
// dispatch work runs in signal context, so Phase D must keep it light or defer
// heavy work.
class SyscallTrap {
public:
    using DispatchFn = std::function<void(const TrapContext&)>;

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

    // PC of the most recent trap handled (diagnostics; also how the smoke
    // test learns where its brk instruction lives). 0 if none yet.
    uint64_t LastTrapPc() const { return last_trap_pc_.load(std::memory_order_acquire); }

    // True while a trap is being processed (re-entrancy guard).
    bool InTrap() const { return in_trap_.load(std::memory_order_acquire); }

private:
    static void SignalTrampoline(int sig, siginfo_t* info, void* uap);
    void HandleTrap(void* uap);

    static SyscallTrap* active_;  // single active instance (M1)

    const std::unordered_map<uint64_t, uint16_t>* table_;
    DispatchFn dispatch_;
    std::atomic<uint64_t> last_trap_pc_{0};
    std::atomic<bool> in_trap_{false};
    bool installed_ = false;
    struct sigaction prev_action_{};
};

} // namespace pnx
