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
}
