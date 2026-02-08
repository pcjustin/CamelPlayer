import SwiftUI

struct PlaybackControlsView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    var body: some View {
        HStack(spacing: 32) {
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
                    .font(.system(size: 64))
            }
            .buttonStyle(.plain)
            .disabled(viewModel.playlistItems.isEmpty)
            .help(viewModel.isPlaying ? "Pause" : "Play")

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

            Spacer()
                .frame(width: 20)

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
        }
        .frame(maxWidth: .infinity)
    }
}
