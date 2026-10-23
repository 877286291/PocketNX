import SwiftUI
import UniformTypeIdentifiers

// Scans a user-chosen directory for NRO / NSO / NXO homebrew files.
// M0: lists files found; M1: adds thumbnails + sort; M5: NCA titles.
struct LibraryView: View {
    var onSelect: (GameItem) -> Void

    @State private var games: [GameItem] = []
    @State private var showingPicker = false
    @State private var errorMessage: String?

    private let extensions = ["nro", "nso", "nxo"]

    var body: some View {
        List {
            if let demo = GameItem.bundledDemo() {
                Button {
                    onSelect(demo)
                } label: {
                    Label(demo.name, systemImage: "play.circle.fill")
                        .foregroundStyle(.blue)
                }
            }
            if games.isEmpty {
                emptyState
            } else {
                ForEach(games) { game in
                    Button {
                        onSelect(game)
                    } label: {
                        Label(game.name, systemImage: "gamecontroller")
                    }
                }
            }
            if let errorMessage {
                Text(errorMessage).foregroundStyle(.red).font(.footnote)
            }
        }
        .navigationTitle("Game Library")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button("Import Homebrew…", systemImage: "plus") {
                    showingPicker = true
                }
            }
        }
        .fileImporter(isPresented: $showingPicker,
                      allowedContentTypes: [.data],
                      allowsMultipleSelection: true) { result in
            switch result {
            case .success(let urls):
                importFiles(urls: urls)
            case .failure(let error):
                errorMessage = error.localizedDescription
            }
        }
    }

    private var emptyState: some View {
        VStack(spacing: 12) {
            Image(systemName: "gamecontroller")
                .font(.system(size: 44))
                .foregroundStyle(.secondary)
            Text("No games yet")
                .font(.title3)
                .foregroundStyle(.secondary)
            Text("Tap + to import homebrew (NRO / NSO / NXO).\nM1 target: a pure-CPU homebrew boots and renders.")
                .font(.footnote)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 60)
    }

    private func importFiles(urls: [URL]) {
        // fileImporter returns security-scoped URLs; copy into app container.
        let container = URL.documentsDirectory
        for url in urls where extensions.contains(url.pathExtension.lowercased()) {
            let dest = container.appendingPathComponent(url.lastPathComponent)
            do {
                try FileManager.default.copyItem(at: url, to: dest)
                games.append(GameItem(url: dest))
            } catch {
                errorMessage = "Import failed: \(error.localizedDescription)"
            }
        }
    }
}
