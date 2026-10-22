#include "os/SvcTable.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "common/Log.h"
#include "os/Services.h"

namespace pnx {

namespace {

// Sentinel prefix for non-service handles (events, transfer memory, ...). Real
// service names are printable ASCII, so byte 0x01 will never collide.
constexpr char kSentinel = '\x01';
const char kSentinelEventW[] = "\x01evnt:W";
const char kSentinelEventR[] = "\x01evnt:R";

// Conservative guard for dereferencing guest pointers. In direct execution the
// guest address space is the host address space, but this still catches the
// most common bad cases (null / absurdly large pointers).
bool IsSafeGuestPtr(uint64_t p) {
    return p != 0 && p < (1ULL << 47);
}

bool ReadGuestCString(uint64_t addr, std::string& out, size_t max_len) {
    if (!IsSafeGuestPtr(addr) || max_len == 0) {
        return false;
    }
    const char* p = reinterpret_cast<const char*>(addr);
    for (size_t i = 0; i < max_len; ++i) {
        char c = p[i];
        if (c == '\0') {
            out.assign(p, i);
            return true;
        }
        out.push_back(c);
    }
    out.assign(p, max_len);
    return true;
}

ResultCode SendSyncRequest(OS& os, const TrapContext& tc, GuestRegs& regs, uint64_t header) {
    const uint32_t handle = static_cast<uint32_t>(regs.x[0]);
    const std::string* kind = os.HandleKind(handle);
    if (!kind || (!kind->empty() && (*kind)[0] == kSentinel)) {
        PNX_LOG_WARN("OS", "SendSyncRequest invalid session handle 0x%x at pc=0x%llx",
                     handle, (unsigned long long)tc.pc);
        regs.x[0] = 0xE401u; // Switch-style invalid-handle error
        return static_cast<ResultCode>(0xE401u);
    }

    // M1 IPC convention: command id is the upper 24 bits of the message
    // header (fast-argument form, x1). Full message-buffer parsing is later.
    const uint32_t command_id = static_cast<uint32_t>((header >> 24) & 0xFFFFFFu);
    Service* svc = os.FindService(*kind);
    if (!svc) {
        PNX_LOG_WARN("OS", "SendSyncRequest unknown service '%s' cmd 0x%x at pc=0x%llx",
                     kind->c_str(), command_id, (unsigned long long)tc.pc);
        regs.x[0] = Unimplemented;
        return Unimplemented;
    }
    return svc->Handle(command_id, tc, regs);
}

} // namespace

void RegisterDefaultSVCs(OS& os) {
    // 0x07 ExitProcess: void. We record it so the runtime can stop the guest.
    os.RegisterSvc(0x07, "ExitProcess", [&os](const TrapContext& tc, GuestRegs& regs) {
        PNX_LOG_INFO("OS", "guest ExitProcess at pc=0x%llx", (unsigned long long)tc.pc);
        os.SetProcessExited(true);
        regs.x[0] = 0;
        return ResultOk;
    });

    // 0x0B SleepThread: x0 = nanoseconds. Negative/zero values mean yield;
    // M1 simply does not sleep. Long sleeps are capped to keep the host alive.
    os.RegisterSvc(0x0B, "SleepThread", [](const TrapContext&, GuestRegs& regs) {
        const int64_t ns = static_cast<int64_t>(regs.x[0]);
        if (ns > 0) {
            constexpr int64_t kMaxNs = 1'000'000'000; // 1 second
            const int64_t sleep_ns = (ns < kMaxNs) ? ns : kMaxNs;
            if (ns != sleep_ns) {
                PNX_LOG_WARN("OS", "SleepThread capped from %lld ns to 1s", (long long)ns);
            }
            std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
        }
        regs.x[0] = 0;
        return ResultOk;
    });

    // 0x10 GetCurrentProcessorNumber: return core 0 for M1's single guest thread.
    os.RegisterSvc(0x10, "GetCurrentProcessorNumber", [](const TrapContext&, GuestRegs& regs) {
        regs.x[0] = 0;
        return ResultOk;
    });

    // Lenient event/signal handlers. Real event state is a later milestone;
    // for M1 these succeed so bring-up is not blocked by synchronization.
    os.RegisterSvc(0x11, "SignalEvent", [](const TrapContext&, GuestRegs& regs) {
        regs.x[0] = ResultOk;
        return ResultOk;
    });
    os.RegisterSvc(0x12, "ClearEvent", [](const TrapContext&, GuestRegs& regs) {
        regs.x[0] = ResultOk;
        return ResultOk;
    });
    os.RegisterSvc(0x17, "ResetSignal", [](const TrapContext&, GuestRegs& regs) {
        regs.x[0] = ResultOk;
        return ResultOk;
    });

    // 0x16 CloseHandle: remove from our handle table if present; lenient on
    // unknown handles to keep bring-up quiet.
    os.RegisterSvc(0x16, "CloseHandle", [&os](const TrapContext&, GuestRegs& regs) {
        const uint32_t handle = static_cast<uint32_t>(regs.x[0]);
        os.ReleaseHandle(handle);
        regs.x[0] = ResultOk;
        return ResultOk;
    });

    // 0x18 WaitSynchronization: M1 returns immediately (index 0) instead of
    // blocking on real event state.
    os.RegisterSvc(0x18, "WaitSynchronization", [](const TrapContext&, GuestRegs& regs) {
        regs.x[0] = ResultOk;
        regs.x[1] = 0;
        return ResultOk;
    });

    // 0x1E GetSystemTick: monotonic nanoseconds.
    os.RegisterSvc(0x1E, "GetSystemTick", [](const TrapContext&, GuestRegs& regs) {
        regs.x[0] = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return ResultOk;
    });

    // 0x1F ConnectToNamedPort: x1 = name (in and out handle register).
    os.RegisterSvc(0x1F, "ConnectToNamedPort", [&os](const TrapContext&, GuestRegs& regs) {
        std::string name;
        if (!ReadGuestCString(regs.x[1], name, 16)) {
            PNX_LOG_WARN("OS", "ConnectToNamedPort bad name ptr 0x%llx", (unsigned long long)regs.x[1]);
            regs.x[0] = InvalidArg;
            return InvalidArg;
        }
        const char* canonical = NormalizeServiceName(name);
        if (!canonical || !os.FindService(canonical)) {
            PNX_LOG_WARN("OS", "ConnectToNamedPort unknown service '%s'", name.c_str());
            regs.x[0] = NotFound;
            return NotFound;
        }
        regs.x[1] = os.AcquireHandle(canonical);
        regs.x[0] = ResultOk;
        PNX_LOG_INFO("OS", "connected to port '%s' -> handle 0x%x", name.c_str(),
                     static_cast<uint32_t>(regs.x[1]));
        return ResultOk;
    });

    // 0x20/0x21 SendSyncRequest(Light): x0 = handle, x1 = message header.
    os.RegisterSvc(0x20, "SendSyncRequestLight",
                   [&os](const TrapContext& tc, GuestRegs& regs) {
                       return SendSyncRequest(os, tc, regs, regs.x[1]);
                   });
    os.RegisterSvc(0x21, "SendSyncRequest",
                   [&os](const TrapContext& tc, GuestRegs& regs) {
                       return SendSyncRequest(os, tc, regs, regs.x[1]);
                   });

    // 0x22 SendSyncRequestWithUserBuffer: x0 = buffer, x1 = size, x2 = handle.
    // M1 only parses the message header (first u64); full buffer args later.
    os.RegisterSvc(0x22, "SendSyncRequestWithUserBuffer",
                   [&os](const TrapContext& tc, GuestRegs& regs) {
                       if (!IsSafeGuestPtr(regs.x[0])) {
                           PNX_LOG_WARN("OS", "SendSyncRequestWithUserBuffer bad buf 0x%llx",
                                        (unsigned long long)regs.x[0]);
                           regs.x[0] = 0xCC01u;
                           return static_cast<ResultCode>(0xCC01u);
                       }
                       uint64_t header = 0;
                       std::memcpy(&header, reinterpret_cast<const void*>(regs.x[0]), 8);
                       regs.x[0] = regs.x[2]; // move handle into the shared path's x0
                       return SendSyncRequest(os, tc, regs, header);
                   });

    // 0x24 GetProcessId / 0x25 GetThreadId: x1 = handle in / id out (M1 fixed IDs).
    os.RegisterSvc(0x24, "GetProcessId", [](const TrapContext&, GuestRegs& regs) {
        regs.x[1] = 0x100;
        regs.x[0] = ResultOk;
        return ResultOk;
    });
    os.RegisterSvc(0x25, "GetThreadId", [](const TrapContext&, GuestRegs& regs) {
        regs.x[1] = 0x1000;
        regs.x[0] = ResultOk;
        return ResultOk;
    });

    // 0x26 Break: diagnostic break, just log and continue.
    os.RegisterSvc(0x26, "Break", [](const TrapContext& tc, GuestRegs&) {
        PNX_LOG_INFO("OS", "guest Break at pc=0x%llx", (unsigned long long)tc.pc);
        return ResultOk;
    });

    // 0x27 OutputDebugString: x0 = str, x1 = len. Primary way M1 homebrew shows text.
    os.RegisterSvc(0x27, "OutputDebugString", [](const TrapContext&, GuestRegs& regs) {
        std::string s;
        const size_t len = (regs.x[1] > 4096) ? 4096 : static_cast<size_t>(regs.x[1]);
        if (IsSafeGuestPtr(regs.x[0]) && len > 0) {
            const char* p = reinterpret_cast<const char*>(regs.x[0]);
            s.assign(p, len);
        }
        PNX_LOG_INFO("OS.debug", "%s", s.c_str());
        regs.x[0] = ResultOk;
        return ResultOk;
    });

    // 0x29 GetInfo: x0 = out*, x1 = type, x2 = handle, x3 = subtype.
    os.RegisterSvc(0x29, "GetInfo", [](const TrapContext&, GuestRegs& regs) {
        uint64_t value = 0;
        const uint32_t type = static_cast<uint32_t>(regs.x[1]);
        switch (type) {
            case 1:  // ProcessId
                value = 0x100;
                break;
            case 2:  // ThreadId
                value = 0x1000;
                break;
            default:
                value = 0;
                break;
        }
        if (!IsSafeGuestPtr(regs.x[0])) {
            PNX_LOG_WARN("OS", "GetInfo bad out ptr 0x%llx", (unsigned long long)regs.x[0]);
            regs.x[0] = InvalidArg;
            return InvalidArg;
        }
        *reinterpret_cast<uint64_t*>(regs.x[0]) = value;
        regs.x[0] = ResultOk;
        return ResultOk;
    });

    // 0x2A FlushEntireDataCache: no-op for M1.
    os.RegisterSvc(0x2A, "FlushEntireDataCache", [](const TrapContext&, GuestRegs&) {
        return ResultOk;
    });

    // 0x45 CreateEvent: x1 = out write-handle*, x2 = out read-handle*.
    os.RegisterSvc(0x45, "CreateEvent", [&os](const TrapContext&, GuestRegs& regs) {
        const uint32_t write_handle = os.AcquireUniqueHandle(kSentinelEventW);
        const uint32_t read_handle = os.AcquireUniqueHandle(kSentinelEventR);
        if (IsSafeGuestPtr(regs.x[1])) {
            *reinterpret_cast<uint32_t*>(regs.x[1]) = write_handle;
        } else {
            regs.x[1] = write_handle; // fallback, not ABI-correct
        }
        if (IsSafeGuestPtr(regs.x[2])) {
            *reinterpret_cast<uint32_t*>(regs.x[2]) = read_handle;
        } else {
            regs.x[2] = read_handle; // fallback, not ABI-correct
        }
        regs.x[0] = ResultOk;
        return ResultOk;
    });
}

void RegisterDefaults(OS& os) {
    RegisterDefaultServices(os);
    RegisterDefaultSVCs(os);
}

} // namespace pnx
