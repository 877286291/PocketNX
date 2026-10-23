#import "EmulatorBridge.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

#include <sys/mman.h>
#include <stdio.h>

#include "common/Log.h"
#include "common/Result.h"
#include "gpu/GPU.h"
#include "runtime/Runtime.h"

// ---------------------------------------------------------------------------
// MetalRenderer: M1 IRenderer. Blits the guest's RGBA8888 framebuffer into
// the current drawable: each present memcpy's the guest pixels into a
// shared-storage Metal texture, and a single full-screen triangle (MSL)
// samples it, aspect-fit letterboxed.
//
// Threading: PresentFramebuffer runs on the MTKView draw callback (main
// thread). The guest thread writes the pixels concurrently; a torn frame is
// an accepted M1 artifact (double buffering lands in M2).
// ---------------------------------------------------------------------------
namespace {

static const char* const kBlitMSL = R"metal(
#include <metal_stdlib>
using namespace metal;

struct BlitOut {
    float4 pos [[position]];
    float2 px;
};

vertex BlitOut blit_vs(uint vid [[vertex_id]],
                       constant float4& drawable [[buffer(0)]]) {
    float2 ndc[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    BlitOut o;
    o.pos = float4(ndc[vid], 0.0, 1.0);
    // Pixel coordinates with top-left origin (both Metal texture rows and
    // guest rows run top-down).
    o.px = float2((ndc[vid].x + 1.0) * 0.5 * drawable.x,
                  (1.0 - (ndc[vid].y + 1.0) * 0.5 * drawable.y));
    return o;
}

fragment float4 blit_fs(BlitOut in [[stage_in]],
                        constant float4& fit [[buffer(0)]],
                        texture2d<half> src [[texture(0)]]) {
    // fit = (offsetX, offsetY, width, height) in drawable pixels; outside
    // the letterbox rect the screen is black.
    bool inside = in.px.x >= fit.x && in.px.x < fit.x + fit.z &&
                  in.px.y >= fit.y && in.px.y < fit.y + fit.w;
    if (!inside) {
        return float4(0.0h, 0.0h, 0.0h, 1.0h);
    }
    float2 uv = (in.px - float2(fit.x, fit.y)) / float2(fit.z, fit.w);
    constexpr sampler s(address::clamp_to_edge, filter::nearest);
    return src.sample(s, uv);
}
)metal";

class MetalRenderer : public pnx::IRenderer {
public:
    MetalRenderer(id<MTLDevice> device, id<MTLCommandQueue> queue)
        : device_(device), queue_(queue) {}

    void SetDrawable(id<MTLTexture> tex) { drawable_ = tex; }

    void BeginFrame() override {}
    void EndFrame() override {}

    pnx::VoidResult PresentFramebuffer(const uint8_t* rgba, uint32_t width,
                                       uint32_t height, uint32_t stride) override {
        if (!drawable_ || !rgba || !queue_ || width == 0 || height == 0) {
            return pnx::VoidResult::Err(pnx::InvalidArg);
        }
        const MTLPixelFormat fmt = [drawable_ pixelFormat];
        if (!EnsurePipeline(fmt) || !EnsureSourceTexture(width, height) ||
            !EnsureUniformBuffer()) {
            return pnx::VoidResult::Err(pnx::Unsupported);
        }

        // 1) Push the guest pixels into the shared-storage source texture.
        // replaceRegion reads straight from the guest buffer (no staging copy);
        // the M1 contract guarantees stride == width * 4 (RGBA8888).
        [src_tex_ replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                     withBytes:rgba
                   bytesPerRow:stride];

        // 2) Aspect-fit letterbox rect, in drawable pixels, written into the
        // shared uniform buffer: [0..15] = VS float4(drawableW, drawableH),
        // [16..31] = FS float4(offsetX, offsetY, rectW, rectH).
        const double dw = static_cast<double>(drawable_.width);
        const double dh = static_cast<double>(drawable_.height);
        const double fit = std::min(dw / double(width), dh / double(height));
        const double rw = double(width) * fit;
        const double rh = double(height) * fit;
        float* u = static_cast<float*>([uniform_buf_ contents]);
        u[0] = float(dw);
        u[1] = float(dh);
        u[4] = float((dw - rw) * 0.5);
        u[5] = float((dh - rh) * 0.5);
        u[6] = float(rw);
        u[7] = float(rh);

        // 3) Render the full-screen triangle into the drawable.
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        if (!cb) {
            return pnx::VoidResult::Err(pnx::Unsupported);
        }
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable_;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (!enc) {
            PNX_LOG_ERROR("Bridge.Metal", "render encoder creation failed");
            return pnx::VoidResult::Err(pnx::Unsupported);
        }
        [enc setRenderPipelineState:pipeline_];
        [enc setVertexBuffer:uniform_buf_ offset:0 atIndex:0];
        [enc setFragmentBuffer:uniform_buf_ offset:16 atIndex:0];
        [enc setFragmentTexture:src_tex_ atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cb commit];
        return pnx::VoidResult::Ok();
    }

private:
    BOOL EnsurePipeline(MTLPixelFormat fmt) {
        if (pipeline_ && pipeline_format_ == fmt) {
            return YES;
        }
        NSError* err = nil;
        id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kBlitMSL]
                                                   options:nil
                                                     error:&err];
        if (!lib) {
            PNX_LOG_ERROR("Bridge.Metal", "MSL compile failed: %s",
                          err.localizedDescription.UTF8String);
            return NO;
        }
        id<MTLFunction> vs = [lib newFunctionWithName:@"blit_vs"];
        id<MTLFunction> fs = [lib newFunctionWithName:@"blit_fs"];
        if (!vs || !fs) {
            PNX_LOG_ERROR("Bridge.Metal", "MSL function lookup failed");
            return NO;
        }
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vs;
        d.fragmentFunction = fs;
        d.colorAttachments[0].pixelFormat = fmt;
        pipeline_ = [device_ newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pipeline_) {
            PNX_LOG_ERROR("Bridge.Metal", "pipeline creation failed: %s",
                          err.localizedDescription.UTF8String);
            return NO;
        }
        pipeline_format_ = fmt;
        return YES;
    }

    BOOL EnsureSourceTexture(uint32_t w, uint32_t h) {
        if (src_tex_ && src_w_ == w && src_h_ == h) {
            return YES;
        }
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        src_tex_ = [device_ newTextureWithDescriptor:td];
        if (!src_tex_) {
            PNX_LOG_ERROR("Bridge.Metal", "source texture alloc failed (%ux%u)", w, h);
            return NO;
        }
        src_w_ = w;
        src_h_ = h;
        return YES;
    }

    BOOL EnsureUniformBuffer() {
        if (uniform_buf_) {
            return YES;
        }
        // 32 bytes: [0..15] vertex-side (drawable size), [16..31]
        // fragment-side (letterbox rect). Written in place every present.
        uniform_buf_ =
            [device_ newBufferWithLength:32 options:MTLResourceStorageModeShared];
        if (!uniform_buf_) {
            PNX_LOG_ERROR("Bridge.Metal", "uniform buffer alloc failed");
            return NO;
        }
        return YES;
    }

    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLTexture> drawable_ = nil;
    id<MTLTexture> src_tex_ = nil;
    uint32_t src_w_ = 0;
    uint32_t src_h_ = 0;
    id<MTLRenderPipelineState> pipeline_ = nil;
    MTLPixelFormat pipeline_format_ = MTLPixelFormatInvalid;
    id<MTLBuffer> uniform_buf_ = nil;
};

} // namespace

// ---------------------------------------------------------------------------
// PNXEmulator: the facade. Owns the C++ Runtime and the guest thread.
//
// Direct-execution model (M1): the guest runs natively on a dedicated
// thread; Runtime::Run() blocks until the guest executes `ret` or calls
// svcExitProcess. The demo guest self-terminates after its frame budget, so
// join is bounded. A never-terminating guest has no interrupt mechanism yet
// (M2: route a stop request through an SVC dispatch hook).
// ---------------------------------------------------------------------------
@implementation PNXEmulator {
    std::shared_ptr<MetalRenderer> renderer_;
    std::unique_ptr<pnx::Runtime> runtime_;
    std::thread guest_thread_;
    // C++20: std::atomic's default constructor value-initializes, so this
    // starts false without an ivar initializer (brace-init in the ivar list
    // is rejected by clang in ObjC++ mode).
    std::atomic<bool> guest_running_;
}

+ (instancetype)shared {
    static PNXEmulator* instance = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ instance = [[PNXEmulator alloc] init]; });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        // Optional debug hook: `-logFile <name>` redirects every PNX_LOG line
        // (fprintf to stderr) into a file in the app's Documents container, so
        // logs can be pulled from the host via
        //   xcrun simctl get_app_container booted com.pocketnx.app data
        // and read at <container>/Documents/<name>. Genuinely useful on device
        // too, where there is no attached console.
        NSArray<NSString *> *args = [[NSProcessInfo processInfo] arguments];
        for (NSUInteger i = 0; i + 1 < args.count; ++i) {
            if ([args[i] isEqualToString:@"-logFile"]) {
                NSString *dir = [NSSearchPathForDirectoriesInDomains(
                    NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
                if (dir.length == 0) {
                    dir = NSTemporaryDirectory();
                }
                NSString *path = [dir stringByAppendingPathComponent:args[i + 1]];
                if (freopen(path.fileSystemRepresentation, "w", stderr) != NULL) {
                    setvbuf(stderr, NULL, _IOLBF, 0);
                }
            }
        }
    }
    return self;
}

- (void)setupWithDevice:(id<MTLDevice>)device queue:(id<MTLCommandQueue>)queue {
    if (!device || !queue) {
        return;
    }
    renderer_ = std::make_shared<MetalRenderer>(device, queue);
    PNX_LOG_INFO("Bridge", "Metal renderer attached");
}

- (void)setDrawableTexture:(id<MTLTexture> _Nullable)texture {
    if (renderer_) {
        renderer_->SetDrawable(texture);
    }
}

- (BOOL)loadGameAtURL:(NSURL *)url error:(NSError **)error {
    if (!url) {
        if (error) {
            *error = [NSError errorWithDomain:@"PNX" code:1
                                     userInfo:@{@"msg": @"nil url"}];
        }
        return NO;
    }

    // Replace any previous guest: wait for its thread, release the runtime.
    if (guest_thread_.joinable()) {
        guest_thread_.join();
    }
    guest_running_ = false;
    runtime_.reset();

    pnx::Runtime::Config cfg;  // Switch-native resolution; the blit fits it.
    cfg.frame_width = 1280;
    cfg.frame_height = 720;
    cfg.max_frames = 0;
    // Optional debug override via launch args, e.g.
    //   xcrun simctl launch booted com.pocketnx.app -autoDemo -frameW 160 -frameH 90
    // Lets the demo run at a small framebuffer, which on the known-affected
    // dev machine (see docs/ENVIRONMENT-ISSUES.md) avoids the store-loop
    // anomaly and gives a clean visual pass of the full pipeline.
    NSArray<NSString *> *launchArgs = [[NSProcessInfo processInfo] arguments];
    for (NSUInteger i = 0; i + 1 < launchArgs.count; ++i) {
        if ([launchArgs[i] isEqualToString:@"-frameW"]) {
            cfg.frame_width = (uint32_t)[launchArgs[i + 1] intValue];
        } else if ([launchArgs[i] isEqualToString:@"-frameH"]) {
            cfg.frame_height = (uint32_t)[launchArgs[i + 1] intValue];
        }
    }
    if (cfg.frame_width == 0) {
        cfg.frame_width = 1280;
    }
    if (cfg.frame_height == 0) {
        cfg.frame_height = 720;
    }
    auto rt = std::make_unique<pnx::Runtime>(cfg);

    auto lr = rt->LoadPath(std::string(url.path.UTF8String));
    if (!lr.IsOk()) {
        if (error) {
            *error = [NSError errorWithDomain:@"PNX" code:2
                                     userInfo:@{@"msg": [NSString stringWithFormat:
                                                         @"NRO load failed (0x%04x)",
                                                         int(lr.code)]}];
        }
        return NO;
    }

    // Prepare rewrites SVC->BRK, mprotects .text to RX (needs the allow-jit
    // entitlement on device), allocates the guest stack/TLS/framebuffer and
    // installs the SIGTRAP handler.
    auto pr = rt->Prepare();
    if (!pr.IsOk()) {
        if (error) {
            *error = [NSError errorWithDomain:@"PNX" code:4
                                     userInfo:@{@"msg": [NSString stringWithFormat:
                                                         @"runtime prepare failed "
                                                          "(0x%04x) - on device this "
                                                          "is usually the missing "
                                                          "allow-jit entitlement",
                                                         int(pr.code)]}];
        }
        return NO;
    }

    runtime_ = std::move(rt);
    PNX_LOG_INFO("Bridge", "loaded %s; framebuffer %ux%u",
                 url.path.UTF8String, cfg.frame_width, cfg.frame_height);
    return YES;
}

- (void)start {
    if (!runtime_ || guest_running_.load(std::memory_order_acquire)) {
        return;
    }
    guest_running_ = true;
    // Capture plain pointers (no `self`): the Runtime outlives this thread
    // (start() joins any prior guest thread; loadGameAtURL() joins before
    // resetting), and the atomic flag is a stable ivar of the singleton.
    pnx::Runtime* rt = runtime_.get();
    std::atomic<bool>* running = &guest_running_;
    guest_thread_ = std::thread([rt, running] {
        auto r = rt->Run();
        if (!r.IsOk()) {
            PNX_LOG_ERROR("Bridge", "guest run failed (0x%04x)", int(r.code));
        }
        *running = false;
    });
}

- (void)stop {
    // M1: demo guests terminate on their own (ret after their last frame), so
    // this join is bounded. See the class comment for the M2 interrupt plan.
    if (guest_thread_.joinable()) {
        guest_thread_.join();
    }
    guest_running_ = false;
}

- (BOOL)isRunning {
    return guest_running_.load(std::memory_order_acquire);
}

- (void)pumpFrame {
    if (!runtime_ || !renderer_) {
        return;
    }
    const auto* fi = runtime_->FrameInfo();
    if (!fi || fi->width == 0 || fi->height == 0) {
        return;
    }
    // The guest writes FramebufferPixels() concurrently from its thread; a
    // torn frame is an accepted M1 artifact (see the renderer comment).
    renderer_->PresentFramebuffer(runtime_->FramebufferPixels(),
                                  fi->width, fi->height, fi->stride);
}

- (uint32_t)guestFrames {
    if (!runtime_) {
        return 0;
    }
    const auto* fi = runtime_->FrameInfo();
    if (!fi) {
        return 0;
    }
    // The guest publishes the counter with a plain STR; a relaxed 64-bit
    // atomic load is a single aligned arm64 instruction (field is 8-aligned).
    // const_cast is needed only because FrameInfo() returns a const pointer;
    // the only writer is the guest (a plain store), so the atomic read is the
    // right host-side primitive.
    const uint64_t c = std::atomic_ref<uint64_t>(const_cast<uint64_t&>(fi->counter))
                           .load(std::memory_order_relaxed);
    return static_cast<uint32_t>(c);
}

- (uint32_t)outputWidth {
    const auto* fi = runtime_ ? runtime_->FrameInfo() : nullptr;
    return (fi && fi->width) ? fi->width : 1280;
}

- (uint32_t)outputHeight {
    const auto* fi = runtime_ ? runtime_->FrameInfo() : nullptr;
    return (fi && fi->height) ? fi->height : 720;
}

- (BOOL)jitEnabled {
    // Direct execution needs executable anonymous memory (mmap RW then
    // mprotect RX). Probe the exact capability instead of reading the
    // entitlement name: on iOS this requires com.apple.security.cs.allow-jit
    // (StikDebug sideload); macOS CLI processes may it implicitly.
    void* p = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NO;
    }
    const bool ok = ::mprotect(p, 4096, PROT_READ | PROT_EXEC) == 0;
    ::munmap(p, 4096);
    return ok;
}

@end
