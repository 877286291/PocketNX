import SwiftUI

// PocketNX app entry. iPad-landscape-first, external-keyboard friendly.
@main
struct PocketNXApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
                .preferredColorScheme(.dark) // emulator UI reads better dark
        }
    }
}

// Root: library by default; entering a game pushes the GameView.
struct ContentView: View {
    @State private var selectedGame: GameItem?

    var body: some View {
        NavigationStack {
            LibraryView(onSelect: { game in selectedGame = game })
                .navigationTitle("PocketNX")
        }
        .fullScreenCover(item: $selectedGame) { game in
            GameView(game: game)
        }
    }
}
