import SwiftUI
import AppKit
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel
    @State private var isDropTargeted = false
    @State private var spaceMonitor: Any?

    var body: some View {
        ZStack {
            VStack(spacing: 0) {
                // Now Playing (left) + Playlist (right)
                HSplitView {
                    NowPlayingView()
                        .padding()
                        .frame(minWidth: 220, idealWidth: 260, maxWidth: 360, maxHeight: .infinity)
                        .background(Color(NSColor.windowBackgroundColor))

                    PlaylistView()
                        .frame(minWidth: 260)
                }
                .frame(minHeight: 240)

                Divider()

                // Transport Bar
                HStack(spacing: 20) {
                    PlaybackControlsView()
                    SeekBarView()
                    VolumeControlView()
                }
                .padding()

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
        .sheet(isPresented: $viewModel.showBrowser) {
            BrowseView()
                .environmentObject(viewModel)
        }
        .onAppear { installSpaceMonitor() }
        .onDisappear {
            if let monitor = spaceMonitor {
                NSEvent.removeMonitor(monitor)
                spaceMonitor = nil
            }
        }
    }

    /// Space toggles play/pause globally, overriding focused-button activation,
    /// except while typing in a text field (e.g. the browse search box).
    private func installSpaceMonitor() {
        guard spaceMonitor == nil else { return }
        spaceMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
            guard event.keyCode == 49 else { return event } // 49 = space
            // Let the space through while typing (e.g. the browse search box).
            if NSApp.keyWindow?.firstResponder is NSText { return event }
            if !viewModel.playlistItems.isEmpty, !viewModel.currentTrackNeedsRenderer {
                viewModel.togglePlayPause()
            }
            // Always swallow space otherwise so it never activates a focused button.
            return nil
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
