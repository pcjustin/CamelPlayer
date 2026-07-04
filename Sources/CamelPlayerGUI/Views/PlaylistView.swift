import SwiftUI

struct PlaylistView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Header
            HStack {
                Text("Playlist")
                    .font(.headline)
                    .padding(.horizontal)
                    .padding(.vertical, 8)

                Spacer()

                Text("\(viewModel.playlistItems.count) tracks")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding(.vertical, 8)

                // Load playlist
                Button(action: { viewModel.loadPlaylist() }) {
                    Image(systemName: "square.and.arrow.down").font(.caption)
                }
                .buttonStyle(.plain)
                .foregroundColor(.secondary)
                .help("Load Playlist")
                .padding(.leading, 8)

                // Save / Clear (only with tracks)
                if !viewModel.playlistItems.isEmpty {
                    Button(action: { viewModel.savePlaylist() }) {
                        Image(systemName: "square.and.arrow.up").font(.caption)
                    }
                    .buttonStyle(.plain)
                    .foregroundColor(.secondary)
                    .help("Save Playlist")
                    .padding(.leading, 8)

                    Button(action: {
                        viewModel.clearPlaylist()
                    }) {
                        Image(systemName: "trash")
                            .font(.caption)
                    }
                    .buttonStyle(.plain)
                    .foregroundColor(.secondary)
                    .help("Clear Playlist")
                    .padding(.horizontal, 8)
                }
            }
            .background(Color(NSColor.controlBackgroundColor))

            Divider()

            // Playlist Content
            if viewModel.playlistItems.isEmpty {
                VStack(spacing: 16) {
                    Spacer()
                    Image(systemName: "music.note.list")
                        .font(.system(size: 48))
                        .foregroundColor(.secondary)
                    Text("No tracks in playlist")
                        .foregroundColor(.secondary)
                    Text("Add files or folders to get started")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
            } else {
                let indexByID = Dictionary(uniqueKeysWithValues:
                    viewModel.playlistItems.enumerated().map { ($1.id, $0) })
                List {
                    // Iterate the Identifiable collection directly (not an
                    // enumerated() wrapper) so macOS List drag-to-reorder works.
                    ForEach(viewModel.playlistItems) { item in
                        let index = indexByID[item.id] ?? 0
                        let isCurrent = viewModel.currentItem?.id == item.id
                        HStack {
                            // Play indicator
                            if isCurrent {
                                Image(systemName: viewModel.isPlaying ? "speaker.wave.2.fill" : "pause.fill")
                                    .foregroundColor(.accentColor)
                                    .font(.caption)
                                    .frame(width: 20)
                            } else {
                                Text("\(index + 1)")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                                    .frame(width: 20)
                            }

                            // Track title
                            Text(item.title)
                                .lineLimit(1)
                                .foregroundColor(isCurrent ? .accentColor : .primary)
                                .font(isCurrent ? .body.weight(.semibold) : .body)

                            Spacer()

                            // Reorder buttons (native List drag is unreliable on macOS)
                            Button {
                                viewModel.movePlaylistItem(fromOffsets: IndexSet(integer: index), toOffset: index - 1)
                            } label: {
                                Image(systemName: "chevron.up").font(.caption)
                            }
                            .buttonStyle(.borderless)
                            .disabled(index == 0)
                            .help("Move up")

                            Button {
                                viewModel.movePlaylistItem(fromOffsets: IndexSet(integer: index), toOffset: index + 2)
                            } label: {
                                Image(systemName: "chevron.down").font(.caption)
                            }
                            .buttonStyle(.borderless)
                            .disabled(index == viewModel.playlistItems.count - 1)
                            .help("Move down")
                        }
                        .contentShape(Rectangle())
                        // Double-click to play.
                        .onTapGesture(count: 2) {
                            viewModel.playItem(at: index)
                        }
                    }
                    .onDelete { indexSet in
                        for index in indexSet {
                            viewModel.removeFromPlaylist(at: index)
                        }
                    }
                    .onMove { from, to in
                        viewModel.movePlaylistItem(fromOffsets: from, toOffset: to)
                    }
                }
                .listStyle(.inset)
            }
        }
    }
}
