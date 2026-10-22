#include "runtime/Runtime.h"

#include <cinttypes>
#include <cstring>

#include "common/Log.h"
#include "os/SvcTable.h"

#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
#include <sys/mman.h>
#endif

namespace pnx {

namespace {

constexpr size_t kPageSize = 0x1000;
inline uint64_t AlignDown(uint64_t v) { return v & ~(uint64_t(kPageSize) - 1); }
inline uint64_t AlignUp(uint64_t v) { return (v + (uint64_t(kPageSize) - 1)) & ~(uint64_t(kPageSize) - 1); }

} // namespace

Runtime::Runtime(Config cfg) : cfg_(cfg) {
    // Register the M1 default services + raw SVC table. Handlers capture `os_`
    // by reference; os_ is a member so it outlives every handler we install.
    RegisterDefaults(os_);
    PNX_LOG_INFO("Runtime", "constructed (base=0x%" PRIx64 " frame=%ux%u)",
                 cfg_.base_address, cfg_.frame_width, cfg_.frame_height);
}

Runtime::~Runtime() {
    if (installed_ && trap_) {
        trap_->Uninstall();
        installed_ = false;
    }
    FreeGuestContext(ctx_);
    TeardownModule();
    PNX_LOG_INFO("Runtime", "destroyed");
}

Result<void> Runtime::LoadPath(const std::string& path) {
    NROLoader loader;
    auto res = loader.Load(path);
    if (!res.IsOk()) {
        PNX_LOG_ERROR("Runtime", "failed to parse '%s' (code=0x%04x)", path.c_str(), res.code);
        return Result<void>::Err(res.code);
    }
    return LoadImage(res.value);
}

Result<void> Runtime::LoadImage(const NROImage& img) {
    TeardownModule();

    if (!img.valid || !img.text.present) {
        PNX_LOG_ERROR("Runtime", "image invalid or missing .text segment");
        return Result<void>::Err(InvalidArg);
    }

    LoadConfig lc;
    lc.base_address = cfg_.base_address;
    lc.make_code_executable = false; // we mprotect ourselves, after rewriting
    Loader ld(lc);
    auto lres = ld.Load(img);
    if (!lres.IsOk()) {
        PNX_LOG_ERROR("Runtime", "Loader::Load failed (code=0x%04x)", lres.code);
        return Result<void>::Err(lres.code);
    }

    image_ = img;
    module_ = std::move(lres.value);
    loaded_ = true;
    PNX_LOG_INFO("Runtime", "loaded image base=0x%" PRIx64 " entry=0x%" PRIx64 " size=0x%zx",
                 module_.base, module_.entry_point, module_.image_size);
    return Result<void>::Ok();
}

Result<void> Runtime::Prepare() {
    if (!loaded_) {
        return Result<void>::Err(InvalidArg);
    }

    // If re-preparing, release the previous guest context (trap was already
    // uninstalled by the caller or by a prior Prepare call).
    FreeGuestContext(ctx_);

    // 1. Allocate the host-owned RGBA8888 framebuffer and describe it to the guest.
    const size_t fb_bytes =
        static_cast<size_t>(cfg_.frame_width) * cfg_.frame_height * 4;
    pixels_.assign(fb_bytes, 0);

    frame_info_ = {};
    frame_info_.width = cfg_.frame_width;
    frame_info_.height = cfg_.frame_height;
    frame_info_.stride = cfg_.frame_width * 4;
    frame_info_.format = 0; // RGBA8888
    frame_info_.capacity = static_cast<uint32_t>(fb_bytes);
    frame_info_.max_frames = cfg_.max_frames; // 0 = guest runs forever
    frame_info_.pixels = reinterpret_cast<uint64_t>(pixels_.data());
    frame_info_.counter = 0;

    // 2. Rewrite SVC -> BRK while .text is still writable.
    uint8_t* host = reinterpret_cast<uint8_t*>(module_.base);
    const uint64_t text_start = image_.text.mem_offset;
    const size_t text_size = static_cast<size_t>(image_.text.size);
    if (text_start + text_size > module_.image_size) {
        PNX_LOG_ERROR("Runtime", "text segment exceeds mapped image bounds");
        return Result<void>::Err(InvalidArg);
    }
    ProgramRewriter rewriter;
    auto rr = rewriter.Rewrite(host + text_start, text_size);
    if (!rr.IsOk()) {
        PNX_LOG_ERROR("Runtime", "ProgramRewriter failed (code=0x%04x)", rr.code);
        return Result<void>::Err(rr.code);
    }
    rewrite_ = rr.value;
    pc_to_svc_ = rr.value.pc_to_svc;

    // 3. Make the pages covering .text executable (RX, not RWX: Apple Silicon
    // enforces W^X — a page cannot be both writable and executable, even with
    // the allow-jit entitlement; the entitlement only permits toggling W<->X).
    //
    // NRO segments are only 256-byte aligned, so a page can in principle be
    // shared between .text and .data/.bss. If that happens we cannot both
    // execute the code and let the guest write its globals, so M1 rejects the
    // layout with a clear error. (Our NRO builder pads segments to page
    // boundaries; real homebrew from the NX SDK does the same in practice.)
    // .rodata sharing a text page is fine: it is only ever read.
    //
    // image_size is page-aligned and text_end <= image_size, so page_end never
    // exceeds the mapping.
    const uint64_t page_start = AlignDown(text_start);
    const uint64_t page_end = AlignUp(text_start + text_size);
    if (image_.data.present) {
        const uint64_t data_lo = image_.data.mem_offset;
        const uint64_t data_hi =
            image_.data.mem_offset + image_.data.size + image_.bss_size;
        if (data_lo < page_end && data_hi > page_start) {
            PNX_LOG_ERROR("Runtime",
                          ".data/.bss (0x%" PRIx64 "-0x%" PRIx64
                          ") shares a page with .text (0x%" PRIx64 "-0x%" PRIx64
                          "); W^X cannot honor both — unsupported NRO layout",
                          data_lo, data_hi, page_start, page_end);
            return Result<void>::Err(Unsupported);
        }
    }
#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
    {
        const int rc = ::mprotect(host + page_start, static_cast<size_t>(page_end - page_start),
                                   PROT_READ | PROT_EXEC);
        if (rc != 0) {
            PNX_LOG_ERROR("Runtime", "mprotect(RX) failed at 0x%" PRIx64 " (len 0x%" PRIx64
                          ") — JIT entitlement likely required",
                          module_.base + page_start, page_end - page_start);
            return Result<void>::Err(Unsupported);
        }
    }
    // Apple Silicon (W^X): instruction-cache maintenance is only guaranteed to
    // take effect once the page is no longer writable, so flush again now that
    // the text page is RX. The ProgramRewriter's earlier flush (while RW) is
    // kept for other platforms and as a cheap first pass.
#if defined(__APPLE__) && defined(__aarch64__)
    __builtin___clear_cache(reinterpret_cast<char*>(host + page_start),
                            reinterpret_cast<char*>(host + page_end));
#endif
#endif

    // 4. Allocate guest stack + TLS; set entry and the framebuffer pointer.
    auto ar = AllocateGuestContext(ctx_, cfg_.stack_size, cfg_.tls_size);
    if (!ar.IsOk()) {
        PNX_LOG_ERROR("Runtime", "AllocateGuestContext failed (code=0x%04x)", ar.code);
        return Result<void>::Err(ar.code);
    }
    ctx_.entry = module_.entry_point;
    ctx_.arg0 = reinterpret_cast<uint64_t>(&frame_info_);

    // 5. Install the SIGTRAP handler and wire the OS dispatch into it.
    trap_ = std::make_unique<SyscallTrap>(pc_to_svc_);
    // The trap must know the guest TLS block to recognize guest traps and
    // run host dispatch code on the host TLS (see SyscallTrap::HandleTrap).
    trap_->SetGuestTlsBlock(reinterpret_cast<uint64_t>(ctx_.tls));
    OS::Wire(*trap_, os_);
    auto ir = trap_->Install();
    if (!ir.IsOk()) {
        PNX_LOG_ERROR("Runtime", "SyscallTrap::Install failed (code=0x%04x)", ir.code);
        trap_.reset();
        return Result<void>::Err(ir.code);
    }
    installed_ = true;
    prepared_ = true;

    PNX_LOG_INFO("Runtime", "prepared: patched %zu SVC -> BRK, %zu pc->svc entries",
                 rewrite_.patched_svcs, pc_to_svc_.size());
    return Result<void>::Ok();
}

Result<void> Runtime::Run() {
    if (!prepared_) {
        return Result<void>::Err(InvalidArg);
    }
    PNX_LOG_INFO("Runtime", "running guest at entry 0x%" PRIx64, module_.entry_point);
    auto jr = JumpToGuest(ctx_);
    PNX_LOG_INFO("Runtime", "guest returned (process_exited=%d)",
                 ProcessExited() ? 1 : 0);
    return jr;
}

void Runtime::TeardownModule() {
#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
    for (auto& r : module_.regions) {
        if (r.addr) {
            ::munmap(r.addr, r.size);
        }
    }
#endif
    module_.regions.clear();
    module_.base = 0;
    module_.entry_point = 0;
    module_.image_size = 0;
    module_.valid = false;
    loaded_ = false;
}

} // namespace pnx
