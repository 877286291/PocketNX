#include "os/OS.h"

#include "common/Log.h"

namespace pnx {

OS::OS() = default;
OS::~OS() = default;

void OS::RegisterSvc(uint32_t svc_num, const char* /*name*/, SvcHandler h) {
    svcs_[svc_num] = std::move(h);
}

bool OS::HasSvc(uint32_t svc_num) const {
    return svcs_.find(svc_num) != svcs_.end();
}

void OS::RegisterService(std::shared_ptr<Service> svc) {
    if (!svc) {
        return;
    }
    const std::string name = svc->Name();
    services_[name] = std::move(svc);
    PNX_LOG_DEBUG("OS", "registered service '%s'", name.c_str());
}

Service* OS::FindService(const std::string& name) const {
    auto it = services_.find(name);
    return it == services_.end() ? nullptr : it->second.get();
}

uint32_t OS::AcquireHandle(const std::string& service_name) {
    // M1 simplification: one stable handle per service. Re-requesting the same
    // service returns the same handle (the real Switch hands out a fresh session
    // each time, but reusing keeps routing deterministic for bring-up).
    for (const auto& [handle, name] : handle_to_service_) {
        if (name == service_name) {
            return handle;
        }
    }
    const uint32_t handle = next_handle_++;
    handle_to_service_[handle] = service_name;
    PNX_LOG_DEBUG("OS", "service handle 0x%x -> '%s'", handle, service_name.c_str());
    return handle;
}

uint32_t OS::AcquireUniqueHandle(const std::string& kind) {
    const uint32_t handle = next_handle_++;
    handle_to_service_[handle] = kind;
    return handle;
}

const std::string* OS::HandleKind(uint32_t handle) const {
    auto it = handle_to_service_.find(handle);
    return it == handle_to_service_.end() ? nullptr : &it->second;
}

bool OS::ReleaseHandle(uint32_t handle) {
    return handle_to_service_.erase(handle) > 0;
}

ResultCode OS::Dispatch(const TrapContext& tc, GuestRegs& regs) {
    last_svc_ = tc.svc_number;
    auto it = svcs_.find(tc.svc_number);
    if (it == svcs_.end()) {
        PNX_LOG_WARN("OS", "unimplemented SVC 0x%02x at pc=0x%llx (x0 = Unimplemented)",
                     tc.svc_number, (unsigned long long)tc.pc);
        regs.x[0] = Unimplemented;
        return Unimplemented;
    }
    return it->second(tc, regs);
}

void OS::Wire(SyscallTrap& trap, OS& os) {
    trap.SetDispatch([&os](const TrapContext& tc, GuestRegs& rg) {
        os.Dispatch(tc, rg);
    });
}

} // namespace pnx
