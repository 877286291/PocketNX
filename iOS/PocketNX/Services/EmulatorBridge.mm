#import "EmulatorBridge.h"

#include <memory>

#include "common/Log.h"
#include "common/Result.h"
#include "cpu/CPU.h"
#include "gpu/GPU.h"
#include "os/OS.h"
#include "fs/NRO.h"

// ---------------------------------------------------------------------------
// MetalRenderer: C++ IRenderer backed by Metal. Presents a guest RGBA8
// framebuffer into the current drawable texture. M1 path (no shader
// translation yet — a raw blit of the guest framebuffer).
// ---------------------------------------------------------------------------
namespace {

class MetalRenderer : public pnx::IRenderer {
public:
    MetalRenderer(id<MTLDevice> device, id<MTLCommandQueue> queue)
        : device_(device), queue_(queue) {
        // A 1-vertex full-screen quad is enough for a framebuffer blit.
        // M2 will replace this with real pipeline state once shaders exist.
    }

    void SetDrawable(id<MTLTexture> tex) { drawable_ = tex; }

    void BeginFrame() override {}
    void EndFrame() override {
        if (pending_) {
            [pending_ commit];
            pending_ = nil;
        }
    }

    pnx::VoidResult PresentFramebuffer(const uint8_t* rgba, uint32_t width,
                                       uint32_t height, uint32_t stride) override {
        if (!drawable_) {
            return pnx::VoidResult::Err(pnx::Result::InvalidArg);
        }
        // M1 blit: build a texture from the guest framebuffer and draw it to the
        // drawable. Full pipeline (vertex/fragment MSL, blend, scaling) lands in
        // M2. For now this is a placeholder commit so the frame path is wired.
        if (queue_) {
            id<MTLCommandBuffer> cb = [queue_ commandBuffer];
            if (cb) {
                [cb commit];
            }
        }
        (void)rgba; (void)width; (void)height; (void)stride;
        return pnx::VoidResult::Ok();
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLTexture> drawable_ = nil;
    id<MTLCommandBuffer> pending_ = nil;
};

} // namespace

// ---------------------------------------------------------------------------
// PNXEmulator: the facade. Owns the C++ engine objects.
// ---------------------------------------------------------------------------
@implementation PNXEmulator {
    std::shared_ptr<pnx::MemoryManager> memory_;
    std::shared_ptr<pnx::CPU> cpu_;
    std::shared_ptr<pnx::OS> os_;
    std::shared_ptr<pnx::GPU> gpu_;
    std::shared_ptr<MetalRenderer> renderer_;
    bool loaded_ = false;
}

+ (instancetype)shared {
    static PNXEmulator* instance = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ instance = [[PNXEmulator alloc] init]; });
    return instance;
}

- (void)setupWithDevice:(id<MTLDevice>)device queue:(id<MTLCommandQueue>)queue {
    if (!device || !queue) {
        return;
    }
    renderer_ = std::make_shared<MetalRenderer>(device, queue);

    // Wire up the C++ engine graph once.
    memory_ = std::make_shared<pnx::MemoryManager>();
    memory_->Initialize(4ULL * 1024 * 1024 * 1024); // 4GB guest view

    pnx::CPU::Config ccfg;
    ccfg.core_count = 4;
    cpu_ = std::make_shared<pnx::CPU>(ccfg);
    cpu_->SetMemory(memory_.get());

    os_ = std::make_shared<pnx::OS>();

    pnx::GPU::Config gcfg;
    gpu_ = std::make_shared<pnx::GPU>(gcfg);
    gpu_->SetRenderer(renderer_);

    PNX_LOG_INFO("Bridge", "engine graph wired (Metal renderer attached)");
}

- (void)setDrawableTexture:(id<MTLTexture> _Nullable)texture {
    if (renderer_) {
        renderer_->SetDrawable(texture);
    }
}

- (BOOL)loadGameAtURL:(NSURL *)url error:(NSError **)error {
    if (!url) {
        if (error) { *error = [NSError errorWithDomain:@"PNX" code:1 userInfo:@{@"msg": @"nil url"}]; }
        return NO;
    }
    pnx::NROLoader loader;
    auto res = loader.Load(url.path.UTF8String);
    if (!res.IsOk()) {
        if (error) {
            *error = [NSError errorWithDomain:@"PNX" code:2
                userInfo:@{@"msg": [NSString stringWithFormat:@"bad NRO (code 0x%x)", res.code]}];
        }
        return NO;
    }
    if (!res.value.valid) {
        if (error) {
            *error = [NSError errorWithDomain:@"PNX" code:3 userInfo:@{@"msg": @"invalid image structure"}];
        }
        return NO;
    }
    // M1: load segments into guest memory, apply relocations, set CPU entry.
    // For M0 we just record success; the CPU entry point wiring lands with
    // the real NSO field extraction.
    loaded_ = res.value.valid;
    PNX_LOG_INFO("Bridge", "loaded game: %s (valid=%d)", res.value.source_path.c_str(), (int)loaded_);
    return YES;
}

- (void)start {
    if (cpu_) {
        cpu_->Run();
    }
}

- (void)stop {
    if (cpu_) {
        cpu_->Stop();
    }
}

- (BOOL)isRunning {
    return cpu_ && cpu_->IsRunning();
}

- (void)pumpFrame {
    // M0: no-op. M1: step the CPU by one frame worth of time, then
    // gpu_->PresentGuestFramebuffer(...) with the current guest front buffer.
    if (gpu_) {
        gpu_->BeginFrame();
        gpu_->EndFrame();
    }
}

- (uint32_t)translatedBlockCount {
    return cpu_ ? static_cast<uint32_t>(cpu_->TranslatedBlockCount()) : 0;
}

- (uint32_t)outputWidth { return gpu_ ? gpu_->OutputWidth() : 1280; }
- (uint32_t)outputHeight { return gpu_ ? gpu_->OutputHeight() : 720; }

@end
