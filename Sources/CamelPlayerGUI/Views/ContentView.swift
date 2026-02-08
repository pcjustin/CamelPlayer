import SwiftUI

struct ContentView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    var body: some View {
        VStack(spacing: 0) {
            // Now Playing
            NowPlayingView()
                .padding()
                .background(Color(NSColor.windowBackgroundColor))

            Divider()

            // Seek Bar
            SeekBarView()
                .padding(.horizontal)
                .padding(.vertical, 8)

            Divider()

            // Playback Controls
            PlaybackControlsView()
                .padding()

            Divider()

            // Volume Control
            VolumeControlView()
                .padding()

            Divider()

            // Playlist
            PlaylistView()
                .frame(minHeight: 200)

            Divider()

            // Settings Bar
            SettingsBarView()
                .padding()
                .background(Color(NSColor.controlBackgroundColor))
        }
        .alert("Error", isPresented: $viewModel.showError) {
            Button("OK") { }
        } message: {
            Text(viewModel.errorMessage ?? "Unknown error")
        }
    }
}
