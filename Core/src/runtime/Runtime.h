#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/Result.h"
#include "cpu/Loader.h"
#include "cpu/ProgramRewriter.h"
#include "cpu/SyscallTrap.h"
#include "cpu/GuestThread.h"
#include "fs/NRO.h"
#include "os/OS.h"
#include "gpu/Framebuffer.h"

namespace pnx {

// The M1 direct-execution runtime: assembles the whole pipeline
//
//     NRO file ─▶ NROLoader (parse) ─▶ Loader (map RW + relocate)
//              ─▶ ProgramRewriter (SVC→BRK, build pc→svc table)
//              ─▶ mprotect(.text, RX) ─▶ SyscallTrap (install) + OS (wire)
//              ─▶ JumpToGuest (guest runs natively on the calling thread)
//
// and owns the shared guest framebuffer (GuestFrameInfo + pixel buffer) that
// the host blits to screen. Under direct execution the guest writes into
// `pixels` directly (same address space); the host reads it back per frame.
//
// M1 scope: single guest thread (the calling thread), one active Runtime.
class Runtime {
public:
    struct Config {
        uint64_t base_address;  // host VA hint for the image (256 MiB default)
        size_t stack_size;      // guest stack, bytes (page-aligned up)
        size_t tls_size;        // guest TLS block, bytes (page-aligned up)
        uint32_t frame_width;   // framebuffer width in pixels
        uint32_t frame_height;  // framebuffer height in pixels
        Config()
            : base_address(0x10000000),
              stack_size(1 << 20),
              tls_size(4096),
              frame_width(1280),
              frame_height(720) {}
    };

    explicit Runtime(Config cfg = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Parse an NRO/NSO/NXO from disk and load it into memory.
    Result<void> LoadPath(const std::string& path);
    // Load an already-parsed image (used by tests to avoid a real file).
    Result<void> LoadImage(const NROImage& img);

    // Patch SVC→BRK, allocate the guest context + framebuffer, install the
    // trap, and wire the OS. Must run after Load*, before Run. Idempotent:
    // re-preparing releases the previous guest context first.
    Result<void> Prepare();

    // Execute the guest on the calling thread. Returns when the guest executes
    // `ret` (returns to the host trampoline). After return the framebuffer is
    // still valid for the host display loop.
    Result<void> Run();

    // True once the guest called svcExitProcess (0x07).
    bool ProcessExited() const { return os_.ProcessExited(); }

    // Host-side accessors for the GPU / display link.
    const GuestFrameInfo* FrameInfo() const { return &frame_info_; }
    const uint8_t* FramebufferPixels() const { return pixels_.data(); }
    size_t FramebufferBytes() const { return pixels_.size(); }

    // Diagnostics / test hooks.
    OS& OsService() { return os_; }
    const OS& OsService() const { return os_; }
    const std::unordered_map<uint64_t, uint16_t>& PCToSvc() const { return pc_to_svc_; }
    const LoadedModule* Module() const { return loaded_ ? &module_ : nullptr; }
    const ProgramRewriteResult& Rewrite() const { return rewrite_; }

private:
    // Release guest stack/TLS and munmap the module regions.
    void TeardownModule();

    Config cfg_;
    OS os_;
    NROImage image_;
    LoadedModule module_{};
    bool loaded_ = false;

    std::vector<uint8_t> pixels_;
    GuestFrameInfo frame_info_{};

    ProgramRewriteResult rewrite_{};
    GuestContext ctx_{};
    // Must outlive trap_ (the trap stores a pointer to this map): declared
    // before trap_ so it is destroyed after it (reverse declaration order).
    std::unordered_map<uint64_t, uint16_t> pc_to_svc_;
    std::unique_ptr<SyscallTrap> trap_;
    bool prepared_ = false;
    bool installed_ = false;
};

} // namespace pnx
