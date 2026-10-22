#pragma once

#include <cstdint>

namespace pnx {

// Shared guest<->host framebuffer contract for the M1 framebuffer-direct path.
//
// Under direct execution the guest and the host share one address space, so the
// `pixels` field is simultaneously a guest pointer and a host pointer: the
// guest renders into it (CPU-rendered homebrew), and the host reads the same
// memory to blit it to the screen (IRenderer::PresentFramebuffer). No copy or
// address translation is needed.
//
// The guest entry point receives a pointer to this struct as x0 (the trampoline
// places GuestContext::arg0 into x0 at entry). The guest MUST only write within
// [pixels, pixels + capacity).
struct GuestFrameInfo {
    uint32_t width;      // framebuffer width in pixels
    uint32_t height;     // framebuffer height in pixels
    uint32_t stride;     // bytes per row (== width * 4 for RGBA8888)
    uint32_t format;     // 0 = RGBA8888 (only supported in M1)
    uint32_t capacity;   // size of the `pixels` buffer in bytes
    uint32_t max_frames; // 0 = render forever; N = stop after N frames and return
    uint64_t pixels;     // guest==host pointer to the RGBA8888 buffer
    uint64_t counter;    // guest-incremented frame counter (host-visible progress)
};

} // namespace pnx
