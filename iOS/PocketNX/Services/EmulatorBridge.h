#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <TargetConditionals.h>

NS_ASSUME_NONNULL_BEGIN

// The single C++ <-> Swift boundary. Swift never sees Core symbols directly;
// everything goes through this ObjC++ facade. The Core (C++20) is built as a
// static library and linked into the app.
//
// Frame flow (M1 direct execution):
//   [emulator start] spawns a guest thread that runs Runtime::Run(); the guest
//   renders into a shared RGBA8888 buffer (guest and host share addresses).
//   Swift MTKView.draw -> setDrawable -> [emulator pumpFrame]
//     -> MetalRenderer::PresentFramebuffer blits the guest buffer (letterboxed)
//     into the current drawable texture.

typedef NS_ENUM(NSInteger, PNXRunState) {
    PNXRunStateStopped = 0,
    PNXRunStateRunning,
};

@interface PNXEmulator : NSObject

+ (instancetype)shared;

// Wire up the Metal backend once the device/queue are available (Swift calls
// this after creating an MTKView).
- (void)setupWithDevice:(id<MTLDevice>)device queue:(id<MTLCommandQueue>)queue;

// Set the drawable the backend renders into and presents (MTKView's
// currentDrawable). Note: as of the iOS 27 SDK, MTLDrawable no longer
// declares `texture` — it moved to CAMetalDrawable (QuartzCore), which
// is also the declared type of MTKView.currentDrawable.
- (void)setDrawable:(id<CAMetalDrawable> _Nullable)drawable;

// Load a homebrew NRO / NSO / NXO by file URL. Returns NO + error on failure.
- (BOOL)loadGameAtURL:(NSURL *)url error:(NSError **)error;

// Lifecycle.
- (void)start;
- (void)stop;
- (BOOL)isRunning;

// Drive one frame from the MTKView. M0: stub; M1: steps the CPU and presents.
- (void)pumpFrame;

// Diagnostics for the Settings UI.
// Frames the guest has published (frame counter from the shared frame info).
- (uint32_t)guestFrames;
- (uint32_t)outputWidth;
- (uint32_t)outputHeight;
// YES when the allow-jit entitlement is present (required for direct
// execution of guest code; StikDebug sideload provides it).
- (BOOL)jitEnabled;

@end

NS_ASSUME_NONNULL_END
