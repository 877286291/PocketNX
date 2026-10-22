import SwiftUI
import MetalKit

// Hosts the Metal view that the Core renders into. MTKView drives the frame
// pump; each draw callback forwards into the C++ engine via the bridge.
struct GameView: View {
    let game: GameItem
    @Environment(\.dismiss) private var dismiss
    @State private var isPaused = true
    @State private var loadError: String?

    var body: some View {
        ZStack(alignment: .topLeading) {
            EmulatorMetalView(isPaused: $isPaused)
                .ignoresSafeArea()

            Button {
                let emu = PNXEmulator.shared()
                emu.stop()
                dismiss()
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.system(size: 30))
                    .foregroundStyle(.white.opacity(0.8))
            }
            .padding()
        }
        .alert("Load Error",
               isPresented: .constant(loadError != nil),
               actions: {
                   Button("OK") {
                       loadError = nil
                       dismiss()
                   }
               }, message: {
                   Text(loadError ?? "")
               })
        .onAppear {
            let emu = PNXEmulator.shared()
            var err: NSError?
            if emu.loadGameAtURL(game.url, error: &err) {
                emu.start()
                isPaused = false // begin the MTKView render loop
            } else {
                loadError = err?.localizedDescription ?? "Unknown error"
            }
        }
        .onDisappear {
            let emu = PNXEmulator.shared()
            emu.stop()
        }
    }
}

// MTKView wrapper. `isPaused` is driven by the Swift layer: paused until the
// game is loaded and started, then unpause to run the continuous render loop.
struct EmulatorMetalView: UIViewRepresentable {
    @Binding var isPaused: Bool

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.colorPixelFormat = .bgra8Unorm
        view.preferredFramesPerSecond = 60
        view.isPaused = true
        view.isMultipleTouchEnabled = false
        view.autoResizeDrawable = true
        view.delegate = context.coordinator
        if let device = view.device, let queue = view.commandQueue {
            PNXEmulator.shared().setupWithDevice(device, queue: queue)
        }
        return view
    }

    func updateUIView(_ uiView: MTKView, context: Context) {
        uiView.isPaused = isPaused
    }

    final class Coordinator: NSObject, MTKViewDelegate {
        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

        func draw(in view: MTKView) {
            let emu = PNXEmulator.shared()
            if let drawable = view.currentDrawable, let texture = drawable.texture {
                emu.setDrawableTexture(texture)
            }
            emu.pumpFrame()
        }
    }
}
