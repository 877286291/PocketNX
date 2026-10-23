import Foundation

// A game / homebrew entry in the library. M0: a file on disk (NRO/NSO/NXO).
// M5: will also model NCA commercial titles.
struct GameItem: Identifiable, Hashable {
    let id: UUID
    let name: String
    let url: URL
    let addedAt: Date

    init(url: URL) {
        self.id = UUID()
        self.name = url.deletingPathExtension().lastPathComponent
        self.url = url
        self.addedAt = Date()
    }

    // Built-in demo shipped in the app bundle, so the M1 acceptance demo
    // (hello_counter) launches with one tap on device/simulator without a
    // file picker. Returns nil if the resource is missing.
    static func bundledDemo() -> GameItem? {
        guard let url = Bundle.main.url(forResource: "hello_counter",
                                        withExtension: "nro") else {
            return nil
        }
        return GameItem(url: url)
    }
}
