#include "cpu/GuestThread.h"

#include <algorithm>
#include <cstring>

#include "common/Log.h"

#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
#include <sys/mman.h>
#endif

namespace pnx {

namespace {
constexpr size_t kPageSize = 0x1000;
inline size_t PageAlign(size_t v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

void* MapAnon(size_t size) {
#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#else
    (void)size;
    return nullptr;
#endif
}

void Unmap(void* p, size_t size) {
#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
    if (p) {
        ::munmap(p, size);
    }
#endif
}

// AArch64 direct-execution trampoline, implemented in GuestThread_arm64.S.
//
// Sets up the guest register state (SP, TPIDR_EL0, x0) and branches to the
// entry point. The guest's return address (x30) is pointed at a local label in
// the .S so that a guest `ret` re-enters the trampoline, which restores the
// host frame and returns. The .S file is used because clang's inline-asm
// cannot resolve an `adr` to a local label (a known inline-asm fixup limit);
// a standalone .S lets the assembler handle the local label directly.
#if defined(__aarch64__)
extern "C" void pnx_guest_trampoline(uint64_t entry, uint64_t arg0,
                                     uint64_t tls, uint64_t stack_top);
#endif

} // namespace

Result<void> AllocateGuestContext(GuestContext& ctx, size_t stack_size, size_t tls_size) {
    const size_t s = PageAlign(stack_size);
    const size_t t = PageAlign(tls_size);
    if (s == 0 || t == 0) {
        return Result<void>::Err(InvalidArg);
    }

    uint8_t* sb = static_cast<uint8_t*>(MapAnon(s));
    if (!sb) {
        PNX_LOG_ERROR("GuestThread", "failed to allocate guest stack (0x%zx)", s);
        return Result<void>::Err(InvalidArg);
    }
    uint8_t* tl = static_cast<uint8_t*>(MapAnon(t));
    if (!tl) {
        Unmap(sb, s);
        PNX_LOG_ERROR("GuestThread", "failed to allocate guest tls (0x%zx)", t);
        return Result<void>::Err(InvalidArg);
    }
    std::memset(tl, 0, t);  // zero the TLS block

    ctx.stack.base = sb;
    ctx.stack.size = s;
    ctx.stack.top = sb + s - 16;  // leave a 16-byte guard at the very top
    ctx.tls = tl;
    ctx.tls_size = t;

    PNX_LOG_INFO("GuestThread", "guest ctx ready: stack=[0x%llx..0x%llx) tls=0x%llx (0x%zx)",
                 reinterpret_cast<uint64_t>(sb), reinterpret_cast<uint64_t>(sb + s),
                 reinterpret_cast<uint64_t>(tl), t);
    return Result<void>::Ok();
}

void FreeGuestContext(GuestContext& ctx) {
    if (ctx.stack.base) {
        Unmap(ctx.stack.base, ctx.stack.size);
    }
    if (ctx.tls) {
        Unmap(ctx.tls, ctx.tls_size);
    }
    ctx = GuestContext{};
}

Result<void> JumpToGuest(const GuestContext& ctx) {
#if defined(__aarch64__)
    if (ctx.entry == 0 || !ctx.stack.top || !ctx.tls) {
        PNX_LOG_ERROR("GuestThread", "invalid guest context (entry/stack/tls)");
        return Result<void>::Err(InvalidArg);
    }
    pnx_guest_trampoline(ctx.entry, ctx.arg0,
                         reinterpret_cast<uint64_t>(ctx.tls),
                         reinterpret_cast<uint64_t>(ctx.stack.top));
    PNX_LOG_INFO("GuestThread", "guest returned to host");
    return Result<void>::Ok();
#else
    (void)ctx;
    PNX_LOG_WARN("GuestThread", "direct execution trampoline requires an AArch64 host");
    return Result<void>::Err(Unsupported);
#endif
}

} // namespace pnx
