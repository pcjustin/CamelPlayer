import SwiftUI
import AppKit
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel
    @State private var isDropTargeted = false
    @State private var spaceMonitor: Any?
    @State private var leftPaneWidth: CGFloat =
        CGFloat(UserDefaults.standard.object(forKey: Self.leftWidthKey) as? Double ?? 260)
    @State private var dragStartWidth: CGFloat?

    private static let leftWidthKey = "ui.leftPaneWidth"
    private let minLeftWidth: CGFloat = 200
    private let maxLeftWidth: CGFloat = 420

    private enum Section { case albums, favorites, recent, browse, queue }
    @State private var section: Section = .albums

    var body: some View {
        ZStack {
            VStack(spacing: 0) {
                // Section switcher — album wall is the default main screen
                Picker("", selection: $section) {
                    Text("Albums").tag(Section.albums)
                    Text("Favorites").tag(Section.favorites)
                    Text("Recent").tag(Section.recent)
                    Text("Browse").tag(Section.browse)
                    Text("Queue").tag(Section.queue)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(maxWidth: 500)
                .padding(8)

                Divider()

                // Section content
                Group {
                    switch section {
                    case .albums: AlbumsView(embedded: true)
                    case .favorites: FavoritesView()
                    case .recent: RecentView()
                    case .browse: BrowseView(embedded: true)
                    case .queue: queueSection
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)

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
        .background(WindowConfigurator(autosaveName: "CamelPlayerMainWindow"))
        .onAppear { installSpaceMonitor() }
        .onDisappear {
            if let monitor = spaceMonitor {
                NSEvent.removeMonitor(monitor)
                spaceMonitor = nil
            }
        }
    }

    /// Now Playing (left) + Playlist (right) with a persisted divider.
    private var queueSection: some View {
        HStack(spacing: 0) {
            NowPlayingView()
                .padding()
                .frame(width: leftPaneWidth)
                .frame(maxHeight: .infinity)
                .background(Color(NSColor.windowBackgroundColor))

            paneDivider

            PlaylistView()
                .frame(minWidth: 260, maxWidth: .infinity)
        }
    }

    /// Draggable divider that resizes and persists the left pane width.
    private var paneDivider: some View {
        ZStack {
            Divider()
            Color.clear
                .frame(width: 8)
                .contentShape(Rectangle())
                .onHover { inside in
                    if inside { NSCursor.resizeLeftRight.push() } else { NSCursor.pop() }
                }
                .gesture(
                    DragGesture()
                        .onChanged { value in
                            if dragStartWidth == nil { dragStartWidth = leftPaneWidth }
                            let proposed = (dragStartWidth ?? leftPaneWidth) + value.translation.width
                            leftPaneWidth = min(maxLeftWidth, max(minLeftWidth, proposed))
                        }
                        .onEnded { _ in
                            dragStartWidth = nil
                            UserDefaults.standard.set(Double(leftPaneWidth), forKey: Self.leftWidthKey)
                        }
                )
        }
        .frame(width: 8)
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
