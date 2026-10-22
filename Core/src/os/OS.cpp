#include "os/OS.h"

#include "common/Log.h"

namespace pnx {

OS::OS() = default;

void OS::RegisterService(std::shared_ptr<Service> svc) {
    if (!svc) {
        return;
    }
    const char* name = svc->Name();
    services_[name] = std::move(svc);
    PNX_LOG_DEBUG("OS", "registered service '%s'", name);
}

Service* OS::FindService(const std::string& name) const {
    auto it = services_.find(name);
    return it == services_.end() ? nullptr : it->second.get();
}

ResultCode OS::Dispatch(uint32_t svc_num, const uint8_t* /*args*/, size_t /*len*/) {
    // M0: the SVC-number -> (module, command id) table is the heart of M1.
    // The mapping is fixed by the NX OS spec; we will build it from a generated
    // header derived from the command-id database. For now everything is stub.
    PNX_LOG_DEBUG("OS", "dispatch SVC 0x%x (stub)", svc_num);
    return Unimplemented;
}

} // namespace pnx
