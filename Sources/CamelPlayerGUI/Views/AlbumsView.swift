import SwiftUI
import CamelPlayerCore

struct AlbumsView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel
    @Environment(\.dismiss) private var dismiss

    var embedded = false
    private let pageSize = 100

    @State private var albums: [MediaObject] = []
    @State private var totalMatches = 0
    @State private var isLoading = false
    @State private var isLoadingMore = false
    @State private var selectedAlbum: MediaObject?

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            content
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .frame(minWidth: embedded ? nil : 640, minHeight: embedded ? nil : 560)
        // Re-run when the library server appears (discovery is async, so it may
        // be nil on first launch and become available a moment later).
        .task(id: viewModel.libraryServer?.id) { await reload() }
    }

    private var header: some View {
        HStack(spacing: 12) {
            if let album = selectedAlbum {
                Button(action: { selectedAlbum = nil }) { Image(systemName: "chevron.left") }
                    .buttonStyle(.borderless)
                Text(album.title).font(.headline).lineLimit(1)
            } else {
                Text("Albums").font(.headline)
                if totalMatches > 0 {
                    Text("\(totalMatches)").font(.caption).foregroundColor(.secondary)
                }
            }
            Spacer()
            if !embedded { Button("Done") { dismiss() } }
        }
        .padding()
    }

    @ViewBuilder
    private var content: some View {
        if let album = selectedAlbum {
            AlbumDetailView(album: album)
        } else if isLoading {
            VStack { Spacer(); ProgressView("Loading albums…"); Spacer() }.frame(maxWidth: .infinity)
        } else if albums.isEmpty {
            VStack(spacing: 12) {
                Spacer()
                Image(systemName: "square.stack").font(.system(size: 40)).foregroundColor(.secondary)
                Text(viewModel.libraryServer == nil ? "No media server found" : "No albums")
                    .foregroundColor(.secondary)
                Spacer()
            }.frame(maxWidth: .infinity)
        } else {
            grid
        }
    }

    private var grid: some View {
        ScrollView {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), spacing: 16)], spacing: 16) {
                ForEach(albums) { album in
                    AlbumCell(album: album)
                        .onTapGesture { selectedAlbum = album }
                        .onAppear { loadMoreIfNeeded(album) }
                }
            }
            .padding()
            if isLoadingMore {
                ProgressView().padding()
            }
        }
    }

    @MainActor
    private func reload() async {
        isLoading = true
        let page = await viewModel.albums(startingIndex: 0, requestedCount: pageSize)
        albums = page?.objects ?? []
        totalMatches = page?.totalMatches ?? albums.count
        isLoading = false
    }

    private func loadMoreIfNeeded(_ album: MediaObject) {
        guard album.id == albums.last?.id, !isLoadingMore, albums.count < totalMatches else { return }
        isLoadingMore = true
        Task {
            let page = await viewModel.albums(startingIndex: albums.count, requestedCount: pageSize)
            if let page = page {
                albums.append(contentsOf: page.objects)
                totalMatches = page.totalMatches
            }
            isLoadingMore = false
        }
    }
}

struct AlbumCell: View {
    @EnvironmentObject var viewModel: PlaybackViewModel
    let album: MediaObject
    @State private var coverURL: URL?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ZStack {
                RoundedRectangle(cornerRadius: 6).fill(Color(NSColor.controlBackgroundColor))
                if let coverURL = coverURL {
                    CachedAsyncImage(url: coverURL) { placeholder }
                } else {
                    placeholder
                }
            }
            .aspectRatio(1, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 6))

            Text(album.title).font(.caption).lineLimit(1)
            if let artist = album.artist, !artist.isEmpty {
                Text(artist).font(.caption2).foregroundColor(.secondary).lineLimit(1)
            }
        }
        .task(id: album.id) { coverURL = await viewModel.albumArtURL(forAlbum: album.id) }
    }

    private var placeholder: some View {
        Image(systemName: "opticaldisc").font(.largeTitle).foregroundColor(.secondary)
    }
}

struct AlbumDetailView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel
    let album: MediaObject

    @State private var tracks: [MediaObject] = []
    @State private var coverURL: URL?

    var body: some View {
        VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 16) {
                ZStack {
                    RoundedRectangle(cornerRadius: 8).fill(Color(NSColor.controlBackgroundColor))
                    if let coverURL = coverURL {
                        CachedAsyncImage(url: coverURL) { Image(systemName: "opticaldisc") }
                    } else {
                        Image(systemName: "opticaldisc").font(.largeTitle).foregroundColor(.secondary)
                    }
                }
                .frame(width: 140, height: 140)
                .clipShape(RoundedRectangle(cornerRadius: 8))

                VStack(alignment: .leading, spacing: 6) {
                    Text(album.title).font(.title3.weight(.semibold)).lineLimit(2)
                    if let artist = album.artist { Text(artist).foregroundColor(.secondary) }
                    Text("\(tracks.count) tracks").font(.caption).foregroundColor(.secondary)
                    HStack(spacing: 10) {
                        Button(action: { viewModel.playAlbum(albumID: album.id) }) {
                            Label("Play Album", systemImage: "play.fill")
                        }
                        .buttonStyle(.borderedProminent)
                        Button(action: { viewModel.toggleFavoriteAlbum(album) }) {
                            Image(systemName: viewModel.isFavoriteAlbum(album.id) ? "star.fill" : "star")
                                .foregroundColor(viewModel.isFavoriteAlbum(album.id) ? .yellow : .secondary)
                        }
                        .buttonStyle(.plain)
                        .help("Favorite album")
                    }
                }
                Spacer()
            }
            .padding()
            Divider()

            List {
                ForEach(Array(tracks.enumerated()), id: \.element.id) { index, track in
                    HStack {
                        Text("\(index + 1)").font(.caption).foregroundColor(.secondary).frame(width: 24)
                        Text(track.title).lineLimit(1)
                        Spacer()
                        if let d = track.duration {
                            Text(TimeFormatter.formatTime(d)).font(.caption).foregroundColor(.secondary)
                        }
                        Button { viewModel.toggleFavoriteTrack(track) } label: {
                            Image(systemName: trackIsFavorite(track) ? "star.fill" : "star")
                                .foregroundColor(trackIsFavorite(track) ? .yellow : .secondary)
                        }
                        .buttonStyle(.borderless)
                        .help("Favorite track")
                        Button { viewModel.addTrack(track) } label: { Image(systemName: "plus.circle") }
                            .buttonStyle(.borderless)
                            .help("Add to playlist")
                    }
                }
            }
        }
        .task(id: album.id) {
            tracks = await viewModel.albumTracks(albumID: album.id)
            coverURL = await viewModel.albumArtURL(forAlbum: album.id)
        }
    }

    private func trackIsFavorite(_ track: MediaObject) -> Bool {
        guard let res = track.resURL else { return false }
        return viewModel.isFavoriteTrack(res)
    }
}
