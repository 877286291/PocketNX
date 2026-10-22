#include "gpu/GPU.h"

#include "common/Log.h"

namespace pnx {

GPU::GPU() : GPU(Config()) {}
GPU::GPU(Config cfg) : cfg_(cfg) {}

GPU::~GPU() = default;

VoidResult GPU::PresentGuestFramebuffer(const uint8_t* rgba, uint32_t width,
                                        uint32_t height, uint32_t stride) {
    if (!renderer_) {
        PNX_LOG_ERROR("GPU", "no renderer attached");
        return VoidResult::Err(InvalidArg);
    }
    if (!rgba) {
        return VoidResult::Err(InvalidArg);
    }
    return renderer_->PresentFramebuffer(rgba, width, height, stride);
}

void GPU::BeginFrame() {
    if (renderer_) {
        renderer_->BeginFrame();
    }
}

void GPU::EndFrame() {
    if (renderer_) {
        renderer_->EndFrame();
    }
}

uint32_t GPU::OutputWidth() const { return cfg_.base_resolution_w * cfg_.scale_factor; }
uint32_t GPU::OutputHeight() const { return cfg_.base_resolution_h * cfg_.scale_factor; }

} // namespace pnx
