import CGtk4
import CamelPlayerCore
import Foundation

// MARK: - Album cell (cover + title + artist)

private func makeAlbumCell(_ model: PlayerModel, album: MediaObject) -> Widget {
    let box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4)
    let cover = CoverView(size: 150, icon: "media-optical-symbolic")
    cp_box_append(box, cover.widget)
    // Cap the natural width so long titles cannot widen the whole
    // homogeneous grid row.
    let title = makeLabel(album.title)
    cp_label_set_max_width_chars(title, 17)
    cp_box_append(box, title)
    if let artist = album.artist, !artist.isEmpty {
        let artistLabel = makeLabel(artist, dim: true)
        cp_label_set_max_width_chars(artistLabel, 17)
        cp_box_append(box, artistLabel)
    }
    gtk_widget_set_size_request(box, 150, -1)
    if let uri = album.albumArtURI, let url = URL(string: uri) {
        cover.setURL(url)
    } else {
        let albumID = album.id
        Task {
            let url = await model.albumArtURL(forAlbum: albumID)
            DispatchQueue.main.async { cover.setURL(url) }
        }
    }
    return box
}

// MARK: - Track row (thumb + title + subtitle + actions)

private func makeTrackRow(
    _ model: PlayerModel,
    title: String,
    subtitle: String?,
    coverURL: URL?,
    duration: TimeInterval? = nil,
    starred: Bool? = nil,
    onStar: (() -> Void)? = nil,
    onAdd: (() -> Void)? = nil
) -> Widget {
    let row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10)
    gtk_widget_set_margin_top(row, 2)
    gtk_widget_set_margin_bottom(row, 2)
    let thumb = CoverView(size: 32)
    thumb.setURL(coverURL)
    cp_box_append(row, thumb.widget)

    let text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2)
    gtk_widget_set_hexpand(text, 1)
    cp_box_append(text, makeLabel(title))
    if let subtitle = subtitle, !subtitle.isEmpty {
        cp_box_append(text, makeLabel(subtitle, dim: true))
    }
    cp_box_append(row, text)

    if let duration = duration {
        cp_box_append(row, makeLabel(TimeFormatter.formatTime(duration), dim: true))
    }
    if let starred = starred, let onStar = onStar {
        let star = gtk_button_new_from_icon_name(starred ? "starred-symbolic" : "non-starred-symbolic")
        cp_button_set_has_frame(star, 0)
        connect(star, "clicked", onStar)
        cp_box_append(row, star)
    }
    if let onAdd = onAdd {
        let add = gtk_button_new_from_icon_name("list-add-symbolic")
        cp_button_set_has_frame(add, 0)
        connect(add, "clicked", onAdd)
        cp_box_append(row, add)
    }
    return row
}

// MARK: - Album detail (shared by Albums / Favorites / Recent)

final class AlbumDetailPane {
    private let model: PlayerModel
    private(set) var root: Widget
    private let cover = CoverView(size: 140, icon: "media-optical-symbolic")
    private var titleLabel: Widget
    private var artistLabel: Widget
    private var countLabel: Widget
    private var starButton: Widget
    private var trackList: Widget
    private var album: MediaObject?
    private var tracks: [MediaObject] = []
    private var loadGeneration = 0

    init(model: PlayerModel) {
        self.model = model
        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)

        let top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16)
        gtk_widget_set_margin_top(top, 8)
        gtk_widget_set_margin_start(top, 8)
        gtk_widget_set_margin_end(top, 8)
        cp_box_append(top, cover.widget)

        let info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6)
        gtk_widget_set_hexpand(info, 1)
        titleLabel = makeLabel("", bold: true)
        artistLabel = makeLabel("", dim: true)
        countLabel = makeLabel("", dim: true)
        cp_box_append(info, titleLabel)
        cp_box_append(info, artistLabel)
        cp_box_append(info, countLabel)

        let actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10)
        let playButton = gtk_button_new_with_label("Play Album")
        gtk_widget_add_css_class(playButton, "suggested-action")
        starButton = gtk_button_new_from_icon_name("non-starred-symbolic")
        cp_button_set_has_frame(starButton, 0)
        cp_box_append(actions, playButton)
        cp_box_append(actions, starButton)
        cp_box_append(info, actions)
        cp_box_append(top, info)
        cp_box_append(root, top)

        cp_box_append(root, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL))

        trackList = gtk_list_box_new()
        cp_list_box_single_click(trackList, 0)
        let scroller = cp_scrolled_window(trackList)
        gtk_widget_set_vexpand(scroller, 1)
        cp_box_append(root, scroller)

        connect(playButton, "clicked") { [weak self] in
            guard let self = self, let album = self.album else { return }
            self.model.playAlbum(album)
        }
        connect(starButton, "clicked") { [weak self] in
            guard let self = self, let album = self.album else { return }
            self.model.toggleFavoriteAlbum(album)
            self.updateStar()
        }
        connectRowActivated(trackList) { [weak self] index in
            guard let self = self, self.tracks.indices.contains(index) else { return }
            self.model.playTrack(self.tracks[index])
        }
    }

    func show(_ album: MediaObject) {
        self.album = album
        loadGeneration += 1
        let generation = loadGeneration
        cp_label_set_markup(titleLabel, "<b>" + markupEscape(album.title) + "</b>")
        cp_label_set_markup(artistLabel, "<span alpha='55%'>" + markupEscape(album.artist ?? "") + "</span>")
        cp_label_set_markup(countLabel, "")
        cover.setURL(nil)
        updateStar()
        tracks = []
        cp_list_box_remove_all(trackList)

        let albumID = album.id
        Task { [weak self] in
            guard let self = self else { return }
            let tracks = await self.model.albumTracks(albumID: albumID)
            let coverURL = await self.model.albumArtURL(forAlbum: albumID)
            DispatchQueue.main.async {
                guard generation == self.loadGeneration else { return }
                self.tracks = tracks
                self.cover.setURL(coverURL)
                self.rebuildTracks()
            }
        }
    }

    private func updateStar() {
        guard let album = album else { return }
        cp_button_set_icon_name(starButton, model.isFavoriteAlbum(album.id)
            ? "starred-symbolic" : "non-starred-symbolic")
    }

    private func rebuildTracks() {
        cp_label_set_markup(countLabel, "<span alpha='55%'>\(tracks.count) tracks</span>")
        cp_list_box_remove_all(trackList)
        for (index, track) in tracks.enumerated() {
            let row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10)
            let number = makeLabel("\(index + 1)", dim: true)
            gtk_widget_set_size_request(number, 24, -1)
            cp_box_append(row, number)
            let title = makeLabel(track.title)
            gtk_widget_set_hexpand(title, 1)
            cp_box_append(row, title)
            if let duration = track.duration {
                cp_box_append(row, makeLabel(TimeFormatter.formatTime(duration), dim: true))
            }
            let starred = track.resURL.map { model.isFavoriteTrack($0) } ?? false
            let star = gtk_button_new_from_icon_name(starred ? "starred-symbolic" : "non-starred-symbolic")
            cp_button_set_has_frame(star, 0)
            let capturedTrack = track
            connect(star, "clicked") { [weak self] in
                self?.model.toggleFavoriteTrack(capturedTrack)
                self?.rebuildTracks()
            }
            cp_box_append(row, star)
            let add = gtk_button_new_from_icon_name("list-add-symbolic")
            cp_button_set_has_frame(add, 0)
            connect(add, "clicked") { [weak self] in self?.model.addTrack(capturedTrack) }
            cp_box_append(row, add)
            cp_list_box_append(trackList, row)
        }
    }
}

// MARK: - List + detail scaffold with a back header

/// Shared scaffold: a header (back button + title + optional extras) above a
/// stack switching between a list page and the album detail page.
class LibraryPaneBase {
    let model: PlayerModel
    private(set) var root: Widget
    let headerExtras: Widget
    private(set) var titleLabel: Widget
    private(set) var listPage: Widget
    private let stack: Widget
    private let backButton: Widget
    let detail: AlbumDetailPane
    private var listTitle = ""

    init(model: PlayerModel, title: String) {
        self.model = model
        listTitle = title
        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0)

        let header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12)
        gtk_widget_set_margin_top(header, 8)
        gtk_widget_set_margin_bottom(header, 8)
        gtk_widget_set_margin_start(header, 12)
        gtk_widget_set_margin_end(header, 12)
        backButton = gtk_button_new_from_icon_name("go-previous-symbolic")
        gtk_widget_set_visible(backButton, 0)
        titleLabel = makeLabel(title, bold: true)
        gtk_widget_set_hexpand(titleLabel, 1)
        headerExtras = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        cp_box_append(header, backButton)
        cp_box_append(header, titleLabel)
        cp_box_append(header, headerExtras)
        cp_box_append(root, header)

        stack = cp_stack_new()
        gtk_widget_set_vexpand(stack, 1)
        listPage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)
        detail = AlbumDetailPane(model: model)
        cp_stack_add(stack, listPage, "list", nil)
        cp_stack_add(stack, detail.root, "detail", nil)
        cp_box_append(root, stack)

        connect(backButton, "clicked") { [weak self] in self?.showList() }
    }

    func setListTitle(_ title: String) {
        listTitle = title
        if gtk_widget_get_visible(backButton) == 0 {
            cp_label_set_markup(titleLabel, "<b>" + markupEscape(title) + "</b>")
        }
    }

    func openDetail(_ album: MediaObject) {
        gtk_widget_set_visible(backButton, 1)
        gtk_widget_set_visible(headerExtras, 0)
        cp_label_set_markup(titleLabel, "<b>" + markupEscape(album.title) + "</b>")
        detail.show(album)
        cp_stack_set_visible(stack, detail.root)
    }

    func showList() {
        gtk_widget_set_visible(backButton, 0)
        gtk_widget_set_visible(headerExtras, 1)
        cp_label_set_markup(titleLabel, "<b>" + markupEscape(listTitle) + "</b>")
        cp_stack_set_visible(stack, listPage)
        listShown()
    }

    /// Hook for subclasses to refresh the list when it becomes visible.
    func listShown() {}
}

// MARK: - Albums pane (album wall + search)

final class AlbumsPane: LibraryPaneBase {
    private var searchEntry: Widget
    private var serverDropdown: Widget
    private var grid: Widget
    private var resultsBox: Widget
    private var resultsAlbumsGrid: Widget
    private var resultsTracksList: Widget
    private var statusLabel: Widget
    private var contentStack: Widget

    private var albums: [MediaObject] = []
    private var totalMatches = 0
    private var isLoadingMore = false
    private var gridPage: Widget
    private var resultsPage: Widget
    private var statusPage: Widget
    private var resultAlbums: [MediaObject] = []
    private var resultTracks: [MediaObject] = []
    private var searchQuery = ""
    private var updatingSearch = false
    private var updatingServers = false
    private var serverIDs: [String] = []
    private var loadedServerID: String?
    private var loadGeneration = 0
    private let pageSize = 100

    init(model: PlayerModel) {
        searchEntry = gtk_search_entry_new()
        serverDropdown = cp_drop_down_new()
        grid = cp_flow_box_new()
        resultsBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)
        resultsAlbumsGrid = cp_flow_box_new()
        resultsTracksList = gtk_list_box_new()
        statusLabel = gtk_label_new("")
        contentStack = cp_stack_new()
        gridPage = cp_scrolled_window(grid)
        resultsPage = cp_scrolled_window(resultsBox)
        statusPage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)
        super.init(model: model, title: "Albums")

        cp_box_append(headerExtras, serverDropdown)

        gtk_widget_set_margin_start(searchEntry, 12)
        gtk_widget_set_margin_end(searchEntry, 12)
        cp_box_append(listPage, searchEntry)

        gtk_widget_set_vexpand(contentStack, 1)
        gtk_widget_set_vexpand(gridPage, 1)
        gtk_widget_set_margin_start(grid, 12)
        gtk_widget_set_margin_end(grid, 12)

        gtk_widget_set_valign(statusPage, GTK_ALIGN_CENTER)
        cp_box_append(statusPage, statusLabel)

        cp_box_append(resultsBox, makeSectionLabel("Albums"))
        gtk_widget_set_margin_start(resultsAlbumsGrid, 12)
        gtk_widget_set_margin_end(resultsAlbumsGrid, 12)
        cp_box_append(resultsBox, resultsAlbumsGrid)
        cp_box_append(resultsBox, makeSectionLabel("Tracks"))
        cp_list_box_single_click(resultsTracksList, 0)
        cp_box_append(resultsBox, resultsTracksList)
        gtk_widget_set_vexpand(resultsPage, 1)

        cp_stack_add(contentStack, gridPage, "grid", nil)
        cp_stack_add(contentStack, resultsPage, "results", nil)
        cp_stack_add(contentStack, statusPage, "status", nil)
        cp_box_append(listPage, contentStack)

        connect(searchEntry, "search-changed") { [weak self] in self?.searchChanged() }
        connectChildActivated(grid) { [weak self] index in
            guard let self = self, self.albums.indices.contains(index) else { return }
            self.openDetail(self.albums[index])
        }
        connectChildActivated(resultsAlbumsGrid) { [weak self] index in
            guard let self = self, self.resultAlbums.indices.contains(index) else { return }
            self.openDetail(self.resultAlbums[index])
        }
        connectRowActivated(resultsTracksList) { [weak self] index in
            guard let self = self, self.resultTracks.indices.contains(index) else { return }
            self.model.playTrack(self.resultTracks[index])
        }
        connectBottomReached(gridPage) { [weak self] in self?.loadMore() }
        connect(serverDropdown, "notify::selected", notify: true) { [weak self] in
            self?.serverSelected()
        }

        showStatus("No media server found")
    }

    /// Reloads when the library server appears or changes (discovery is async).
    func refreshIfNeeded() {
        refreshServerDropdown()
        guard model.libraryServer?.id != loadedServerID else { return }
        reload()
    }

    private func makeSectionLabel(_ text: String) -> Widget {
        let label = makeLabel(text, bold: true)
        gtk_widget_set_margin_start(label, 12)
        gtk_widget_set_margin_top(label, 8)
        return label
    }

    private func showStatus(_ text: String) {
        cp_label_set_text(statusLabel, text)
        cp_stack_set_visible(contentStack, statusPage)
    }

    private func refreshServerDropdown() {
        let servers = model.mediaServers
        gtk_widget_set_visible(serverDropdown, servers.count > 1 ? 1 : 0)
        let ids = servers.map { $0.id }
        guard ids != serverIDs else { return }
        serverIDs = ids
        updatingServers = true
        let list = cp_string_list_new()
        for server in servers { cp_string_list_append(list, server.friendlyName) }
        cp_drop_down_set_model(serverDropdown, list)
        if let index = servers.firstIndex(where: { $0.id == model.libraryServer?.id }) {
            cp_drop_down_set_selected(serverDropdown, UInt32(index))
        }
        updatingServers = false
    }

    private func serverSelected() {
        guard !updatingServers else { return }
        let index = Int(cp_drop_down_get_selected(serverDropdown))
        guard serverIDs.indices.contains(index) else { return }
        model.setLibraryServer(serverIDs[index])
        reload()
    }

    private func reload() {
        loadGeneration += 1
        let generation = loadGeneration
        loadedServerID = model.libraryServer?.id
        guard model.libraryServer != nil else {
            showStatus("No media server found")
            return
        }
        showStatus("Loading albums...")
        albums = []
        Task { [weak self] in
            guard let self = self else { return }
            let page = await self.model.albums(startingIndex: 0, requestedCount: self.pageSize)
            DispatchQueue.main.async {
                guard generation == self.loadGeneration else { return }
                self.albums = page?.objects ?? []
                self.totalMatches = page?.totalMatches ?? self.albums.count
                self.setListTitle("Albums (\(self.totalMatches))")
                self.rebuildGrid()
            }
        }
    }

    private func loadMore() {
        guard !isLoadingMore, !albums.isEmpty, albums.count < totalMatches,
              searchQuery.isEmpty else { return }
        isLoadingMore = true
        let generation = loadGeneration
        Task { [weak self] in
            guard let self = self else { return }
            let page = await self.model.albums(startingIndex: self.albums.count,
                                               requestedCount: self.pageSize)
            DispatchQueue.main.async {
                self.isLoadingMore = false
                guard generation == self.loadGeneration, let page = page else { return }
                for album in page.objects {
                    self.albums.append(album)
                    cp_flow_box_append(self.grid, makeAlbumCell(self.model, album: album))
                }
                self.totalMatches = page.totalMatches
            }
        }
    }

    private func rebuildGrid() {
        guard !albums.isEmpty else {
            showStatus(model.libraryServer == nil ? "No media server found" : "No albums")
            return
        }
        cp_flow_box_remove_all(grid)
        for album in albums {
            cp_flow_box_append(grid, makeAlbumCell(model, album: album))
        }
        cp_stack_set_visible(contentStack, gridPage)
    }

    private func searchChanged() {
        guard !updatingSearch else { return }
        let text = String(cString: cp_editable_get_text(searchEntry))
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard text != searchQuery else { return }
        searchQuery = text
        if text.isEmpty {
            rebuildGrid()
            return
        }
        let generation = loadGeneration + 1
        loadGeneration = generation
        showStatus("Searching...")
        Task { [weak self] in
            guard let self = self else { return }
            let objects = await self.model.searchLibrary(query: text)
            DispatchQueue.main.async {
                guard generation == self.loadGeneration else { return }
                self.resultAlbums = objects.filter { $0.isContainer }
                self.resultTracks = objects.filter { !$0.isContainer }
                self.rebuildResults()
            }
        }
    }

    private func rebuildResults() {
        guard !resultAlbums.isEmpty || !resultTracks.isEmpty else {
            showStatus("No matches")
            return
        }
        cp_flow_box_remove_all(resultsAlbumsGrid)
        for album in resultAlbums {
            cp_flow_box_append(resultsAlbumsGrid, makeAlbumCell(model, album: album))
        }
        cp_list_box_remove_all(resultsTracksList)
        for track in resultTracks {
            let capturedTrack = track
            let row = makeTrackRow(
                model, title: track.title, subtitle: track.album,
                coverURL: track.albumArtURI.flatMap { URL(string: $0) },
                onAdd: { [weak self] in self?.model.addTrack(capturedTrack) })
            cp_list_box_append(resultsTracksList, row)
        }
        cp_stack_set_visible(contentStack, resultsPage)
    }

    override func listShown() {
        refreshServerDropdown()
        setListTitle(totalMatches > 0 ? "Albums (\(totalMatches))" : "Albums")
    }
}

// MARK: - Favorites / Recent panes

/// Shared list of album refs (grid) + track refs (list).
class RefsPane: LibraryPaneBase {
    private var albumsGrid: Widget
    private var tracksList: Widget
    private var statusLabel: Widget
    private var statusPage: Widget
    private var scroller: Widget
    private var contentStack: Widget
    private var albumRefs: [AlbumRef] = []
    private var trackRefs: [TrackRef] = []

    var emptyMessage: String { "Nothing here yet" }
    var refs: (albums: [AlbumRef], tracks: [TrackRef]) { ([], []) }
    var trackStarFilled: Bool { false }

    override init(model: PlayerModel, title: String) {
        albumsGrid = cp_flow_box_new()
        tracksList = gtk_list_box_new()
        statusLabel = gtk_label_new("")
        statusPage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)
        contentStack = cp_stack_new()
        let content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)
        scroller = cp_scrolled_window(content)
        super.init(model: model, title: title)

        gtk_widget_set_vexpand(contentStack, 1)
        gtk_widget_set_vexpand(scroller, 1)

        let albumsLabel = makeLabel("Albums", bold: true)
        gtk_widget_set_margin_start(albumsLabel, 12)
        cp_box_append(content, albumsLabel)
        gtk_widget_set_margin_start(albumsGrid, 12)
        gtk_widget_set_margin_end(albumsGrid, 12)
        cp_box_append(content, albumsGrid)
        let tracksLabel = makeLabel("Tracks", bold: true)
        gtk_widget_set_margin_start(tracksLabel, 12)
        gtk_widget_set_margin_top(tracksLabel, 8)
        cp_box_append(content, tracksLabel)
        cp_list_box_single_click(tracksList, 0)
        cp_box_append(content, tracksList)

        gtk_widget_set_valign(statusPage, GTK_ALIGN_CENTER)
        cp_box_append(statusPage, statusLabel)

        cp_stack_add(contentStack, scroller, "content", nil)
        cp_stack_add(contentStack, statusPage, "status", nil)
        cp_box_append(listPage, contentStack)

        connectChildActivated(albumsGrid) { [weak self] index in
            guard let self = self, self.albumRefs.indices.contains(index) else { return }
            self.openDetail(self.model.openAlbumRef(self.albumRefs[index]))
        }
        connectRowActivated(tracksList) { [weak self] index in
            guard let self = self, self.trackRefs.indices.contains(index) else { return }
            self.model.playTrack(self.trackRefs[index])
        }
    }

    /// Star button action for a track row; default is unfavorite-style removal.
    func starAction(_ ref: TrackRef) {}

    func rebuild() {
        let current = refs
        albumRefs = current.albums
        trackRefs = current.tracks
        guard !albumRefs.isEmpty || !trackRefs.isEmpty else {
            cp_label_set_text(statusLabel, emptyMessage)
            cp_stack_set_visible(contentStack, statusPage)
            return
        }
        cp_flow_box_remove_all(albumsGrid)
        for ref in albumRefs {
            cp_flow_box_append(albumsGrid, makeAlbumCell(model, album: model.openAlbumRef(ref)))
        }
        cp_list_box_remove_all(tracksList)
        for ref in trackRefs {
            let capturedRef = ref
            let row = makeTrackRow(
                model, title: ref.title, subtitle: ref.album,
                coverURL: ref.albumArtURI.flatMap { URL(string: $0) },
                starred: trackStarFilled ? true : nil,
                onStar: trackStarFilled ? { [weak self] in
                    self?.starAction(capturedRef)
                    self?.rebuild()
                } : nil,
                onAdd: { [weak self] in self?.model.addTrack(capturedRef) })
            cp_list_box_append(tracksList, row)
        }
        cp_stack_set_visible(contentStack, scroller)
    }

    override func listShown() {
        rebuild()
    }
}

final class FavoritesPane: RefsPane {
    override var emptyMessage: String { "No favorites yet. Star an album or track to add it here." }
    override var refs: (albums: [AlbumRef], tracks: [TrackRef]) {
        (model.favoriteAlbums, model.favoriteTracks)
    }
    override var trackStarFilled: Bool { true }

    init(model: PlayerModel) {
        super.init(model: model, title: "Favorites")
    }

    override func starAction(_ ref: TrackRef) {
        model.unfavoriteTrack(ref)
    }
}

final class RecentPane: RefsPane {
    override var emptyMessage: String { "Nothing played yet" }
    override var refs: (albums: [AlbumRef], tracks: [TrackRef]) {
        (model.recentAlbums, model.recentTracks)
    }

    init(model: PlayerModel) {
        super.init(model: model, title: "Recent")
        let clear = gtk_button_new_with_label("Clear")
        connect(clear, "clicked") { [weak self] in
            self?.model.clearRecentlyPlayed()
            self?.rebuild()
        }
        cp_box_append(headerExtras, clear)
    }
}
