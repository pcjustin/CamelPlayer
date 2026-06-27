import SwiftUI
import CamelPlayerCore

struct RecentView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel
    @State private var selectedAlbum: MediaObject?

    var body: some View {
        if let album = selectedAlbum {
            VStack(spacing: 0) {
                HStack(spacing: 12) {
                    Button(action: { selectedAlbum = nil }) { Image(systemName: "chevron.left") }
                        .buttonStyle(.borderless)
                    Text(album.title).font(.headline).lineLimit(1)
                    Spacer()
                }
                .padding()
                Divider()
                AlbumDetailView(album: album)
            }
        } else {
            list
        }
    }

    @ViewBuilder
    private var list: some View {
        if viewModel.recentAlbums.isEmpty && viewModel.recentTracks.isEmpty {
            VStack(spacing: 12) {
                Spacer()
                Image(systemName: "clock").font(.system(size: 40)).foregroundColor(.secondary)
                Text("Nothing played yet").foregroundColor(.secondary)
                Spacer()
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            ScrollView {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Spacer()
                        Button("Clear") { viewModel.clearRecentlyPlayed() }.font(.caption)
                    }
                    .padding(.horizontal)

                    if !viewModel.recentAlbums.isEmpty {
                        Text("Albums").font(.headline).padding(.horizontal)
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), spacing: 16)], spacing: 16) {
                            ForEach(viewModel.recentAlbums) { ref in
                                AlbumCell(album: viewModel.openFavoriteAlbum(ref))
                                    .onTapGesture { selectedAlbum = viewModel.openFavoriteAlbum(ref) }
                            }
                        }
                        .padding(.horizontal)
                    }

                    if !viewModel.recentTracks.isEmpty {
                        Text("Tracks").font(.headline).padding(.horizontal).padding(.top, 8)
                        ForEach(viewModel.recentTracks) { ref in
                            HStack(spacing: 10) {
                                trackThumb(ref)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(ref.title).lineLimit(1)
                                    if let album = ref.album, !album.isEmpty {
                                        Text(album).font(.caption).foregroundColor(.secondary).lineLimit(1)
                                    }
                                }
                                Spacer()
                                Button { viewModel.addTrack(ref) } label: { Image(systemName: "plus.circle") }
                                    .buttonStyle(.borderless).help("Add to playlist")
                            }
                            .padding(.horizontal)
                            .contentShape(Rectangle())
                            .onTapGesture(count: 2) { viewModel.playTrack(ref) }
                        }
                    }
                }
                .padding(.vertical)
            }
        }
    }

    @ViewBuilder
    private func trackThumb(_ ref: TrackRef) -> some View {
        let fallback = Image(systemName: "music.note").foregroundColor(.secondary)
        if let s = ref.albumArtURI, let url = URL(string: s) {
            CachedAsyncImage(url: url) { fallback }
                .frame(width: 32, height: 32)
                .clipShape(RoundedRectangle(cornerRadius: 4))
        } else {
            fallback.frame(width: 32, height: 32)
        }
    }
}
