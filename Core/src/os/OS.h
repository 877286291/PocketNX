#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "common/Result.h"

namespace pnx {

// A guest process handle context. M0: minimal identity.
struct ProcessContext {
    uint64_t process_id = 0;
    uint64_t code_address = 0;
    uint64_t entry_point = 0;
    uint64_t stack_address = 0;
};

// One NX OS service (hid, sfc, sm, ...). Implementations return ResultCode for
// each guest command id. Unimplemented commands should return Result::Unimplemented.
class Service {
public:
    virtual ~Service() = default;
    virtual const char* Name() const = 0;
    // command id -> ResultCode. `args` is the guest-passed argument buffer.
    virtual ResultCode Handle(uint32_t command_id, const uint8_t* args, size_t len) = 0;
};

// The OS layer: registry of services + the SVC dispatch entry the CPU calls.
class OS {
public:
    OS();

    // Register a service object under its name (e.g. "hid", "sfc").
    void RegisterService(std::shared_ptr<Service> svc);

    // Look up a service by name.
    Service* FindService(const std::string& name) const;

    // CPU SVC dispatch: map svc number -> (module, command). M0: stub.
    ResultCode Dispatch(uint32_t svc_num, const uint8_t* args, size_t len);

private:
    std::unordered_map<std::string, std::shared_ptr<Service>> services_;
};

} // namespace pnx
