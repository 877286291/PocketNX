#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "common/Result.h"
#include "cpu/SyscallTrap.h"

namespace pnx {

// A guest process handle context. M1: minimal identity.
struct ProcessContext {
    uint64_t process_id = 0;
    uint64_t code_address = 0;
    uint64_t entry_point = 0;
    uint64_t stack_address = 0;
};

// One guest OS service (hid, sfc, sm, sfd, ...). Reached via inter-process
// communication (svcSendSyncRequest). A Service dispatches a single IPC command
// (command id) by reading the guest registers and writing the result back.
// Unimplemented commands should return `Unimplemented` (surfaced to the guest
// as a non-zero Result in x0) and are logged for homebrew bring-up.
class Service {
public:
    virtual ~Service() = default;
    virtual const char* Name() const = 0;
    // Handle one guest command. Read arguments from `regs` (x0..x7 are the
    // IPC fast-argument registers) and write the guest-visible Result to
    // `regs.x[0]`. Return the same ResultCode for diagnostics.
    virtual ResultCode Handle(uint32_t command_id, const TrapContext& tc,
                              GuestRegs& regs) = 0;
};

// The OS layer: a command-dispatch table for raw system calls (SVC numbers),
// a registry of named IPC services, and a guest handle table mapping service
// handles to services (for svcSendSyncRequest routing).
//
// Dispatch model (matches the Switch and the "direct execution" approach):
//   * The ProgramRewriter rewrites each `svc #N` to a BRK and records the
//     original SVC number N in a PC->svc side table.
//   * The SyscallTrap reads the faulting PC, resolves it to SVC number N, and
//     calls OS::Dispatch with (trap, regs).
//   * OS::Dispatch looks N up in the raw-SVC table and runs the handler, which
//     reads args from regs.x[0..7] and writes the guest result to regs.x[0].
//   * For service calls (svcSendSyncRequest), the handler routes by the guest
//     handle (x0) to the target Service and dispatches the command id.
class OS {
public:
    // Handler for one raw SVC number. Reads args from regs.x[0..7]; writes the
    // guest-visible Result to regs.x[0]; returns the same ResultCode.
    using SvcHandler = std::function<ResultCode(const TrapContext&, GuestRegs&)>;

    OS();
    ~OS();

    OS(const OS&) = delete;
    OS& operator=(const OS&) = delete;

    // Register a raw SVC handler by SVC number. `name` is for logging only.
    void RegisterSvc(uint32_t svc_num, const char* name, SvcHandler h);

    // True if a handler is registered for `svc_num`.
    bool HasSvc(uint32_t svc_num) const;

    // Register a named IPC service (e.g. "hid", "sfc").
    void RegisterService(std::shared_ptr<Service> svc);

    // Look up a service by name.
    Service* FindService(const std::string& name) const;

    // Guest handle table. In direct execution a handle is just an integer the
    // homebrew keeps; we map it back to a kind string (a canonical service name
    // for service handles, or a sentinel for events / transfer memory).
    //
    // Service handles reuse one stable handle per service (M1 simplification);
    // the Switch would hand out a fresh session each time.
    uint32_t AcquireHandle(const std::string& service_name);
    // Always allocates a fresh handle (events, transfer memory, ...).
    uint32_t AcquireUniqueHandle(const std::string& kind);
    const std::string* HandleKind(uint32_t handle) const;
    bool ReleaseHandle(uint32_t handle);

    // Primary dispatch entry, called from the SyscallTrap. Routes `tc.svc_number`
    // through the raw-SVC table. Unknown numbers return `Unimplemented` (logged)
    // and leave x0 = Unimplemented so the homebrew can see the failure.
    ResultCode Dispatch(const TrapContext& tc, GuestRegs& regs);

    // Plug this OS into a SyscallTrap so trapped BRKs dispatch here.
    static void Wire(SyscallTrap& trap, OS& os);

    // Set when a handler observed svcExitProcess (guest wants to stop).
    bool ProcessExited() const { return process_exited_; }
    void SetProcessExited(bool v) { process_exited_ = v; }

    // Diagnostics: the last raw SVC number dispatched (for logging/tests).
    uint32_t LastSvc() const { return last_svc_; }

private:
    std::unordered_map<std::string, std::shared_ptr<Service>> services_;
    std::unordered_map<uint32_t, SvcHandler> svcs_;
    std::unordered_map<uint32_t, std::string> handle_to_service_;
    uint32_t next_handle_ = 0x100;
    bool process_exited_ = false;
    uint32_t last_svc_ = 0;
};

} // namespace pnx
