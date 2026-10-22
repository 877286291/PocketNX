import SwiftUI

// Diagnostics + tuning. The JIT status indicator is critical on iOS: without
// the JIT entitlement (StikDebug) the CPU can't translate, and performance
// collapses. Surfacing it early saves users hours of confusion.
struct SettingsView: View {
    @State private var guestFrames: UInt32 = 0
    @State private var outW: UInt32 = 1280
    @State private var outH: UInt32 = 720

    var body: some View {
        Form {
            Section("Status") {
                LabeledContent("Running", value: isRunning ? "Yes" : "No")
                LabeledContent("JIT enabled", value: jitEnabled ? "Yes" : "No (check StikDebug)")
                    .foregroundStyle(jitEnabled ? .primary : .red)
                LabeledContent("Guest frames", value: "\(guestFrames)")
            }

            Section("Render") {
                LabeledContent("Output", value: "\(outW) × \(outH)")
            }

            Section("About") {
                LabeledContent("Version", value: "0.0.1 (M1 direct-exec)")
                LabeledContent("Route", value: "B — from scratch")
                Text("PocketNX does not bundle Nintendo firmware, keys, or ROMs. Provide your own.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Settings")
        .onAppear(perform: refresh)
    }

    private var isRunning: Bool { PNXEmulator.shared().isRunning() }
    // Direct execution needs the allow-jit entitlement (StikDebug sideload);
    // the bridge queries the process entitlements directly.
    private var jitEnabled: Bool { PNXEmulator.shared().jitEnabled() }

    private func refresh() {
        let emu = PNXEmulator.shared()
        guestFrames = emu.guestFrames()
        outW = emu.outputWidth()
        outH = emu.outputHeight()
    }
}
