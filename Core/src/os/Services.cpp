#include "os/Services.h"

#include <cstring>
#include <string>

namespace pnx {

namespace {

// Lowercase the part of `raw` before any ':' so that "smd:" and "smd" both
// normalize to the same key.
std::string KeyOf(const std::string& raw) {
    std::string name;
    for (char c : raw) {
        if (c == ':') {
            break;
        }
        name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return name;
}

} // namespace

const char* NormalizeServiceName(const std::string& raw) {
    using AliasEntry = const std::pair<const char*, const char*>;
    static const AliasEntry aliases[] = {
        {"sm", "sm"},
        {"sm:m", "sm"},
        {"sm:u", "sm"},
        {"smd", "sm"},
        {"hid", "hid"},
        {"hid:sys", "hid"},
        {"hid:dbg", "hid"},
        {"hidm", "hid"},
        {"hidinf", "hid"},
        {"sfc", "sfc"},
        {"sfd", "sfd"},
    };

    const std::string key = KeyOf(raw);
    for (const auto& entry : aliases) {
        if (key == entry.first) {
            return entry.second;
        }
    }
    return nullptr;
}

void RegisterDefaultServices(OS& os) {
    // sm (service manager): the key service — homebrew fetches every other
    // service handle through #GetServiceHandle.
    auto sm = std::make_shared<CommandService>("sm");
    // Cmd 0: #RegisterClient — accept, nothing to do in M1.
    sm->Register(0, [](const TrapContext&, GuestRegs& regs) {
        regs.x[0] = ResultOk;
        return ResultOk;
    });
    // Cmd 1: #GetServiceHandle — x2 holds the ServiceName (8 zero-padded chars
    // packed in a u64); return a handle for that service in x1, result in x0.
    sm->Register(1, [&os](const TrapContext&, GuestRegs& regs) {
        uint64_t name_bits = regs.x[2];
        char buf[9] = {0};
        std::memcpy(buf, &name_bits, 8);
        const char* canonical = NormalizeServiceName(buf);
        if (!canonical || !os.FindService(canonical)) {
            PNX_LOG_WARN("OS.sm", "GetServiceHandle: unknown service");
            regs.x[0] = ResultCode{NotFound};
            return NotFound;
        }
        regs.x[1] = os.AcquireHandle(canonical);
        regs.x[0] = ResultOk;
        return ResultOk;
    });
    os.RegisterService(std::move(sm));

    // hid (input): stub — the activate-family commands succeed with a dummy
    // resource handle; state queries are not modeled in M1.
    auto hid = std::make_shared<CommandService>("hid");
    const auto activate_stub = [](const TrapContext&, GuestRegs& regs) {
        regs.x[1] = 0;  // dummy resource handle
        regs.x[0] = ResultOk;
        return ResultOk;
    };
    hid->Register(0, activate_stub);  // CreateAppletResource
    hid->Register(1, activate_stub);  // ActivateDebugPad
    hid->Register(2, activate_stub);  // ActivateDefault
    hid->Register(3, activate_stub);  // ActivateNpad
    hid->Register(11, activate_stub); // ActivateTouchScreen
    os.RegisterService(std::move(hid));

    // sfc (storage) / sfd (save data): present but minimal in M1; unimplemented
    // commands surface as Unimplemented (logged) during homebrew bring-up.
    os.RegisterService(std::make_shared<CommandService>("sfc"));
    os.RegisterService(std::make_shared<CommandService>("sfd"));
}

} // namespace pnx
