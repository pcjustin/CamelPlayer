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
    @State private var searchText = ""
    @State private var searchQuery = ""
    @State private var resultAlbums: [MediaObject] = []
    @State private var resultTracks: [MediaObject] = []

    private var isSearching: Bool { !searchQuery.isEmpty }

    var body: some View {
        VStack(spacing: 0) {
            header
            if selectedAlbum == nil { searchField }
            Divider()
            content
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .frame(minWidth: embedded ? nil : 640, minHeight: embedded ? nil : 560)
        // Re-run when the library server appears (discovery is async, so it may
        // be nil on first launch and become available a moment later).
        .task(id: viewModel.libraryServer?.id) { await reload() }
    }

    private var searchField: some View {
        HStack(spacing: 6) {
            Image(systemName: "magnifyingglass").foregroundColor(.secondary)
            TextField("Search albums, artists, songs", text: $searchText)
                .textFieldStyle(.plain)
                .onSubmit(runSearch)
            if !searchText.isEmpty {
                Button(action: clearSearch) { Image(systemName: "xmark.circle.fill") }
                    .buttonStyle(.plain).foregroundColor(.secondary)
            }
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color(NSColor.controlBackgroundColor)))
        .padding(.horizontal).padding(.bottom, 8)
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
        } else if isSearching {
            searchResults
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

    @ViewBuilder
    private var searchResults: some View {
        if resultAlbums.isEmpty && resultTracks.isEmpty {
            VStack(spacing: 12) {
                Spacer()
                Image(systemName: "magnifyingglass").font(.system(size: 40)).foregroundColor(.secondary)
                Text("No matches").foregroundColor(.secondary)
                Spacer()
            }.frame(maxWidth: .infinity)
        } else {
            ScrollView {
                VStack(alignment: .leading, spacing: 8) {
                    if !resultAlbums.isEmpty {
                        Text("Albums").font(.headline).padding(.horizontal)
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), spacing: 16)], spacing: 16) {
                            ForEach(resultAlbums) { album in
                                AlbumCell(album: album)
                                    .onTapGesture { selectedAlbum = album }
                            }
                        }
                        .padding(.horizontal)
                    }
                    if !resultTracks.isEmpty {
                        Text("Tracks").font(.headline).padding(.horizontal).padding(.top, 8)
                        ForEach(resultTracks) { track in
                            HStack(spacing: 10) {
                                trackThumb(track)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(track.title).lineLimit(1)
                                    if let album = track.album, !album.isEmpty {
                                        Text(album).font(.caption).foregroundColor(.secondary).lineLimit(1)
                                    }
                                }
                                Spacer()
                                Button { viewModel.addTrack(track) } label: { Image(systemName: "plus.circle") }
                                    .buttonStyle(.borderless).help("Add to playlist")
                            }
                            .padding(.horizontal)
                            .contentShape(Rectangle())
                            .onTapGesture(count: 2) { viewModel.playTrack(track) }
                        }
                    }
                }
                .padding(.vertical)
            }
        }
    }

    private func trackThumb(_ track: MediaObject) -> some View {
        let fallback = Image(systemName: "music.note").foregroundColor(.secondary)
        return Group {
            if let s = track.albumArtURI, let url = URL(string: s) {
                CachedAsyncImage(url: url) { fallback }
            } else {
                fallback
            }
        }
        .frame(width: 32, height: 32)
        .clipShape(RoundedRectangle(cornerRadius: 4))
    }

    private func runSearch() {
        let query = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !query.isEmpty else { return }
        searchQuery = query
        resultAlbums = []
        resultTracks = []
        Task {
            let objects = await viewModel.searchLibrary(query: query)
            resultAlbums = objects.filter { $0.isContainer }
            resultTracks = objects.filter { !$0.isContainer }
        }
    }

    private func clearSearch() {
        searchText = ""
        searchQuery = ""
        resultAlbums = []
        resultTracks = []
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
                        Button(action: { viewModel.playAlbum(album) }) {
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
                    .contentShape(Rectangle())
                    .onTapGesture(count: 2) { viewModel.playTrack(track) }
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
