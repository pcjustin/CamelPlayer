import SwiftUI

/// Menu-bar commands that also provide global keyboard shortcuts for playback.
struct PlaybackCommands: Commands {
    @ObservedObject var viewModel: PlaybackViewModel

    var body: some Commands {
        CommandMenu("Playback") {
            // Space is handled by a key monitor in ContentView (a bare-space
            // menu shortcut gets eaten by whichever button has focus).
            Button(viewModel.isPlaying ? "Pause" : "Play") { viewModel.togglePlayPause() }
                .disabled(viewModel.playlistItems.isEmpty || viewModel.currentTrackNeedsRenderer)

            Divider()

            Button("Next") { viewModel.next() }
                .keyboardShortcut(.rightArrow, modifiers: .command)
                .disabled(!viewModel.canGoNext)
            Button("Previous") { viewModel.previous() }
                .keyboardShortcut(.leftArrow, modifiers: .command)
                .disabled(!viewModel.canGoPrevious)
            Button("Stop") { viewModel.stop() }
                .keyboardShortcut(".", modifiers: .command)
                .disabled(viewModel.isStopped)

            Divider()

            Button("Seek Forward") { seek(by: 5) }
                .keyboardShortcut(.rightArrow, modifiers: [])
            Button("Seek Backward") { seek(by: -5) }
                .keyboardShortcut(.leftArrow, modifiers: [])

            Divider()

            Button("Volume Up") { viewModel.setVolume(min(1, viewModel.volume + 0.05)) }
                .keyboardShortcut(.upArrow, modifiers: .command)
            Button("Volume Down") { viewModel.setVolume(max(0, viewModel.volume - 0.05)) }
                .keyboardShortcut(.downArrow, modifiers: .command)
        }
    }

    private func seek(by delta: TimeInterval) {
        guard let duration = viewModel.duration else { return }
        viewModel.seek(to: max(0, min(duration, viewModel.currentTime + delta)))
    }
}
