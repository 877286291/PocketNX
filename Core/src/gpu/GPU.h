#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/Result.h"

namespace pnx {

// Abstract render backend interface. Core is platform-independent, so the GPU
// talks to this; the iOS app supplies a Metal implementation. This decoupling
// lets us develop on Mac with a null/preview backend.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Present a client framebuffer (raw RGBA8) to the screen.
    // Used by the M1 framebuffer-direct backend before shader translation exists.
    virtual VoidResult PresentFramebuffer(const uint8_t* rgba, uint32_t width,
                                          uint32_t height, uint32_t stride) = 0;

    // Per-frame begin/end (command buffer scope).
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
};

// GPU front-end. Parses Switch graphics-engine command buffers and drives a
// backend. M0: structure only; M1 uses the framebuffer path, M2 adds real
// shader translation (Maxwell -> MSL) on top of the same front-end.
class GPU {
public:
    struct Config {
        uint32_t base_resolution_w = 1280; // Switch native
        uint32_t base_resolution_h = 720;
        uint32_t scale_factor = 1;         // resolution scaling (M4)
    };

    GPU();
    explicit GPU(Config cfg);
    ~GPU();

    void SetRenderer(std::shared_ptr<IRenderer> r) { renderer_ = std::move(r); }
    IRenderer* Renderer() const { return renderer_.get(); }

    // M1: blit the guest framebuffer straight to screen (no shader translation).
    VoidResult PresentGuestFramebuffer(const uint8_t* rgba, uint32_t width,
                                       uint32_t height, uint32_t stride);

    // M2: begin/end a real frame driven by parsed command buffers.
    void BeginFrame();
    void EndFrame();

    uint32_t OutputWidth() const;
    uint32_t OutputHeight() const;

private:
    Config cfg_;
    std::shared_ptr<IRenderer> renderer_;
};

} // namespace pnx
