import SwiftUI

// Diagnostics + tuning. The JIT status indicator is critical on iOS: without
// the JIT entitlement (StikDebug) the CPU can't translate, and performance
// collapses. Surfacing it early saves users hours of confusion.
struct SettingsView: View {
    @State private var blockCount: UInt32 = 0
    @State private var outW: UInt32 = 1280
    @State private var outH: UInt32 = 720

    var body: some View {
        Form {
            Section("Status") {
                LabeledContent("Running", value: isRunning ? "Yes" : "No")
                LabeledContent("JIT enabled", value: jitEnabled ? "Yes" : "No (check StikDebug)")
                    .foregroundStyle(jitEnabled ? .primary : .red)
                LabeledContent("Translated blocks", value: "\(blockCount)")
            }

            Section("Render") {
                LabeledContent("Output", value: "\(outW) × \(outH)")
            }

            Section("About") {
                LabeledContent("Version", value: "0.0.1 (M0 skeleton)")
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
    // M0: JIT detection lands when the CPU actually probes mach ports.
    private var jitEnabled: Bool { false }

    private func refresh() {
        let emu = PNXEmulator.shared()
        blockCount = emu.translatedBlockCount()
        outW = emu.outputWidth()
        outH = emu.outputHeight()
    }
}
