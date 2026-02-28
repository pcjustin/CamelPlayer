import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel
    @State private var isDropTargeted = false

    var body: some View {
        ZStack {
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

            // Drop overlay
            if isDropTargeted {
                RoundedRectangle(cornerRadius: 12)
                    .strokeBorder(Color.accentColor, lineWidth: 3)
                    .background(Color.accentColor.opacity(0.08).clipShape(RoundedRectangle(cornerRadius: 12)))
                    .overlay {
                        VStack(spacing: 12) {
                            Image(systemName: "plus.circle.fill")
                                .font(.system(size: 48))
                                .foregroundColor(.accentColor)
                            Text("Drop to Add to Playlist")
                                .font(.title3.weight(.semibold))
                                .foregroundColor(.accentColor)
                        }
                    }
                    .padding(8)
                    .allowsHitTesting(false)
            }
        }
        .onDrop(of: [UTType.fileURL], isTargeted: $isDropTargeted) { providers in
            handleDrop(providers: providers)
            return true
        }
        .alert("Error", isPresented: $viewModel.showError) {
            Button("OK") { }
        } message: {
            Text(viewModel.errorMessage ?? "Unknown error")
        }
    }

    private func handleDrop(providers: [NSItemProvider]) {
        var resolvedURLs: [URL] = []
        let group = DispatchGroup()

        for provider in providers {
            group.enter()
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
                defer { group.leave() }
                guard let data = item as? Data,
                      let url = URL(dataRepresentation: data, relativeTo: nil) else { return }
                var isDirectory: ObjCBool = false
                FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory)
                if isDirectory.boolValue {
                    let files = FilePickerHelper.scanFolder(url)
                    resolvedURLs.append(contentsOf: files)
                } else {
                    let audioExtensions = ["mp3", "wav", "m4a", "flac", "alac", "aac", "aiff"]
                    if audioExtensions.contains(url.pathExtension.lowercased()) {
                        resolvedURLs.append(url)
                    }
                }
            }
        }

        group.notify(queue: .main) {
            guard !resolvedURLs.isEmpty else { return }
            viewModel.addFiles(resolvedURLs)
        }
    }
}
