import SwiftUI

struct PlaybackControlsView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    var body: some View {
        HStack(spacing: 18) {
            // Shuffle
            Button(action: {
                viewModel.setShuffle(!viewModel.shuffle)
            }) {
                Image(systemName: "shuffle")
                    .font(.title3)
                    .foregroundColor(viewModel.shuffle ? .accentColor : .secondary)
            }
            .buttonStyle(.plain)
            .help("Shuffle")

            // Previous
            Button(action: {
                viewModel.previous()
            }) {
                Image(systemName: "backward.fill")
                    .font(.title2)
            }
            .buttonStyle(.plain)
            .disabled(!viewModel.canGoPrevious)
            .help("Previous Track")

            // Play/Pause
            Button(action: {
                if viewModel.isPlaying {
                    viewModel.pause()
                } else if viewModel.isPaused {
                    viewModel.resume()
                } else {
                    viewModel.play()
                }
            }) {
                Image(systemName: viewModel.isPlaying ? "pause.circle.fill" : "play.circle.fill")
                    .font(.system(size: 44))
            }
            .buttonStyle(.plain)
            .disabled(viewModel.playlistItems.isEmpty || viewModel.currentTrackNeedsRenderer)
            .help(viewModel.currentTrackNeedsRenderer
                  ? "Select a network renderer to play this network track"
                  : (viewModel.isPlaying ? "Pause" : "Play"))

            // Next
            Button(action: {
                viewModel.next()
            }) {
                Image(systemName: "forward.fill")
                    .font(.title2)
            }
            .buttonStyle(.plain)
            .disabled(!viewModel.canGoNext)
            .help("Next Track")

            // Stop
            Button(action: {
                viewModel.stop()
            }) {
                Image(systemName: "stop.fill")
                    .font(.title2)
            }
            .buttonStyle(.plain)
            .disabled(viewModel.isStopped)
            .help("Stop")

            // Loop (off → all → one)
            Button(action: {
                viewModel.cycleLoop()
            }) {
                Image(systemName: viewModel.loopMode == .one ? "repeat.1" : "repeat")
                    .font(.title3)
                    .foregroundColor(viewModel.loopMode == .off ? .secondary : .accentColor)
            }
            .buttonStyle(.plain)
            .help("Loop: off / all / one")
        }
        .fixedSize()
    }
}
