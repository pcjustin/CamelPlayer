import SwiftUI
import CamelPlayerCore

struct FavoritesView: View {
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
        if viewModel.favoriteAlbums.isEmpty && viewModel.favoriteTracks.isEmpty {
            VStack(spacing: 12) {
                Spacer()
                Image(systemName: "star").font(.system(size: 40)).foregroundColor(.secondary)
                Text("No favorites yet").foregroundColor(.secondary)
                Text("Star an album or track to add it here")
                    .font(.caption).foregroundColor(.secondary)
                Spacer()
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            ScrollView {
                VStack(alignment: .leading, spacing: 8) {
                    if !viewModel.favoriteAlbums.isEmpty {
                        Text("Albums").font(.headline).padding(.horizontal)
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), spacing: 16)], spacing: 16) {
                            ForEach(viewModel.favoriteAlbums) { ref in
                                AlbumCell(album: viewModel.openFavoriteAlbum(ref))
                                    .onTapGesture { selectedAlbum = viewModel.openFavoriteAlbum(ref) }
                            }
                        }
                        .padding(.horizontal)
                    }

                    if !viewModel.favoriteTracks.isEmpty {
                        Text("Tracks").font(.headline).padding(.horizontal).padding(.top, 8)
                        ForEach(viewModel.favoriteTracks) { ref in
                            HStack(spacing: 10) {
                                trackThumb(ref)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(ref.title).lineLimit(1)
                                    if let album = ref.album, !album.isEmpty {
                                        Text(album).font(.caption).foregroundColor(.secondary).lineLimit(1)
                                    }
                                }
                                Spacer()
                                Button { viewModel.unfavoriteTrack(ref) } label: {
                                    Image(systemName: "star.fill").foregroundColor(.yellow)
                                }
                                .buttonStyle(.borderless).help("Remove from favorites")
                                Button { viewModel.addTrack(ref) } label: {
                                    Image(systemName: "plus.circle")
                                }
                                .buttonStyle(.borderless).help("Add to playlist")
                            }
                            .padding(.horizontal)
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
