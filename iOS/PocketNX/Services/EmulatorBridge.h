#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>

NS_ASSUME_NONNULL_BEGIN

// The single C++ <-> Swift boundary. Swift never sees Core symbols directly;
// everything goes through this ObjC++ facade. The Core (C++20) is built as a
// static library and linked into the app.
//
// Frame flow (M1+):
//   Swift MTKView.draw -> [emulator pumpFrame]
//     -> C++ advances the guest CPU -> on vsync calls GPU::PresentGuestFramebuffer
//     -> MetalBackend::PresentFramebuffer -> draws into the current drawable texture

typedef NS_ENUM(NSInteger, PNXRunState) {
    PNXRunStateStopped = 0,
    PNXRunStateRunning,
};

@interface PNXEmulator : NSObject

+ (instancetype)shared;

// Wire up the Metal backend once the device/queue are available (Swift calls
// this after creating an MTKView).
- (void)setupWithDevice:(id<MTLDevice>)device queue:(id<MTLCommandQueue>)queue;

// Swap the drawable texture the backend presents into (MTKView drawable).
- (void)setDrawableTexture:(id<MTLTexture> _Nullable)texture;

// Load a homebrew NRO / NSO / NXO by file URL. Returns NO + error on failure.
- (BOOL)loadGameAtURL:(NSURL *)url error:(NSError **)error;

// Lifecycle.
- (void)start;
- (void)stop;
- (BOOL)isRunning;

// Drive one frame from the MTKView. M0: stub; M1: steps the CPU and presents.
- (void)pumpFrame;

// Diagnostics for the Settings UI.
- (uint32_t)translatedBlockCount;
- (uint32_t)outputWidth;
- (uint32_t)outputHeight;

@end

NS_ASSUME_NONNULL_END
