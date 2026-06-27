import SwiftUI

@main
struct CamelPlayerGUIApp: App {
    @StateObject private var viewModel = PlaybackViewModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(viewModel)
                .frame(minWidth: 600, minHeight: 560)
        }
        .windowStyle(.hiddenTitleBar)
        .commands {
            PlaybackCommands(viewModel: viewModel)
        }
    }
}
