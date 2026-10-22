#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "common/Log.h"
#include "os/OS.h"

namespace pnx {

// Service base that dispatches by IPC command id. Commands that are not
// registered return `Unimplemented` (surfaced to the guest as a non-zero Result
// in x0) and are logged — per ARCHITECTURE §4, this is the fastest way to see
// which services a homebrew needs during bring-up.
class CommandService : public Service {
public:
    using CmdHandler = std::function<ResultCode(const TrapContext&, GuestRegs&)>;

    explicit CommandService(const char* name) : name_(name) {}

    const char* Name() const override { return name_; }

    void Register(uint32_t command_id, CmdHandler h) { cmds_[command_id] = std::move(h); }
    bool HasCommand(uint32_t command_id) const { return cmds_.find(command_id) != cmds_.end(); }

    ResultCode Handle(uint32_t command_id, const TrapContext& tc, GuestRegs& regs) override {
        auto it = cmds_.find(command_id);
        if (it == cmds_.end()) {
            PNX_LOG_WARN("OS", "unimplemented '%s' command 0x%x at pc=0x%llx",
                         name_, command_id, (unsigned long long)tc.pc);
            regs.x[0] = Unimplemented;
            return Unimplemented;
        }
        return it->second(tc, regs);
    }

private:
    const char* name_;
    std::unordered_map<uint32_t, CmdHandler> cmds_;
};

// Map a guest service/port name (possibly carrying a ':' suffix or an alias,
// e.g. "smd:", "hid:sys", "sm:m") to the canonical name of a registered
// Service. Returns nullptr if the name is unknown.
const char* NormalizeServiceName(const std::string& raw);

// Register the M1 default services as Service objects under their canonical
// names. Per ARCHITECTURE §4: sm (service manager) and hid (input) are the
// functional stubs; sfc (storage) and sfd (save data) are present but minimal.
void RegisterDefaultServices(OS& os);

} // namespace pnx
