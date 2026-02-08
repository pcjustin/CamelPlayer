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

                // Clear button
                if !viewModel.playlistItems.isEmpty {
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
                List {
                    ForEach(Array(viewModel.playlistItems.enumerated()), id: \.offset) { index, item in
                        HStack {
                            // Play indicator
                            if index == viewModel.currentPosition {
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
                                .foregroundColor(index == viewModel.currentPosition ? .accentColor : .primary)
                                .font(index == viewModel.currentPosition ? .body.weight(.semibold) : .body)

                            Spacer()
                        }
                        .contentShape(Rectangle())
                        .onTapGesture {
                            viewModel.playItem(at: index)
                        }
                    }
                    .onDelete { indexSet in
                        for index in indexSet {
                            viewModel.removeFromPlaylist(at: index)
                        }
                    }
                }
                .listStyle(.inset)
            }
        }
    }
}
