import CGtk4
import CamelPlayerCore
import Foundation

/// UPnP media server browser: server list -> folder navigation -> tracks.
/// Mirrors the macOS BrowseView including the sort menu; no album art thumbs.
final class BrowsePane {
    private let model: PlayerModel
    private(set) var root: Widget

    private var headerLabel: Widget
    private var addFolderButton: Widget
    private var searchEntry: Widget
    private var infoLabel: Widget
    private var sortDropdown: Widget
    private var listBox: Widget

    private var server: UPnPDevice?
    private var path: [(id: String, title: String)] = []
    private var objects: [MediaObject] = []
    private var serverRows: [UPnPDevice] = []
    private var searchQuery = ""
    private var updatingSearch = false
    private var sortCaps: [String] = []
    private var sortCriteria = ""
    private var sortOptions: [(label: String, criteria: String)] = []
    private var updatingSort = false
    /// Discards results of superseded loads.
    private var loadGeneration = 0

    private var showingServers: Bool { server == nil }

    init(model: PlayerModel) {
        self.model = model

        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)
        gtk_widget_set_margin_top(root, 12)
        gtk_widget_set_margin_bottom(root, 12)
        gtk_widget_set_margin_start(root, 12)
        gtk_widget_set_margin_end(root, 12)

        let header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        let backButton = gtk_button_new_from_icon_name("go-previous-symbolic")
        headerLabel = makeLabel("Network Servers", bold: true)
        gtk_widget_set_hexpand(headerLabel, 1)
        addFolderButton = gtk_button_new_with_label("Add Folder")
        cp_box_append(header, backButton)
        cp_box_append(header, headerLabel)
        cp_box_append(header, addFolderButton)
        cp_box_append(root, header)

        searchEntry = gtk_search_entry_new()
        cp_box_append(root, searchEntry)

        let infoRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        infoLabel = gtk_label_new("")
        cp_label_ellipsize_end(infoLabel)
        cp_label_set_xalign(infoLabel, 0)
        gtk_widget_set_hexpand(infoLabel, 1)
        sortDropdown = cp_drop_down_new()
        gtk_widget_set_visible(sortDropdown, 0)
        cp_box_append(infoRow, infoLabel)
        cp_box_append(infoRow, sortDropdown)
        cp_box_append(root, infoRow)

        listBox = gtk_list_box_new()
        let scroller = cp_scrolled_window(listBox)
        gtk_widget_set_vexpand(scroller, 1)
        cp_box_append(root, scroller)

        connect(backButton, "clicked") { [weak self] in self?.goBack() }
        connect(addFolderButton, "clicked") { [weak self] in self?.addCurrentFolder() }
        connect(searchEntry, "search-changed") { [weak self] in self?.searchChanged() }
        connectRowActivated(listBox) { [weak self] index in self?.rowActivated(index) }
        connect(sortDropdown, "notify::selected", notify: true) { [weak self] in
            self?.sortSelected()
        }

        showServers()
    }

    /// Called when discovery changes the server list (already on the GTK thread).
    func serversChanged() {
        if showingServers { showServers() }
    }

    private func setInfo(_ text: String) {
        cp_label_set_text(infoLabel, text)
    }

    private func setHeader(_ text: String) {
        cp_label_set_markup(headerLabel, "<b>" + markupEscape(text) + "</b>")
    }

    private func showServers() {
        server = nil
        path = []
        objects = []
        sortCaps = []
        sortCriteria = ""
        loadGeneration += 1
        gtk_widget_set_visible(sortDropdown, 0)
        setHeader("Network Servers")
        serverRows = model.mediaServers
        cp_list_box_remove_all(listBox)
        for device in serverRows {
            cp_list_box_append_label(listBox, "\(device.friendlyName)  (\(device.modelName))")
        }
        setInfo(serverRows.isEmpty
            ? "No media servers found. Make sure your NAS is on the same network."
            : "Select a server")
    }

    private func rowActivated(_ index: Int) {
        if showingServers {
            guard serverRows.indices.contains(index) else { return }
            open(serverRows[index])
            return
        }
        guard objects.indices.contains(index) else { return }
        let object = objects[index]
        if object.isContainer {
            descend(into: object)
        } else if model.controller.addTrackToPlaylist(object) {
            setInfo("Added: \(object.title)")
        }
    }

    private func open(_ device: UPnPDevice) {
        server = device
        path = [(id: "0", title: device.friendlyName)]
        clearSearchField()
        sortCriteria = ""
        Task { [weak self] in
            let caps = await self?.model.controller.sortCapabilities(server: device) ?? []
            DispatchQueue.main.async { self?.applySortCaps(caps) }
        }
        reload()
    }

    // Same known-field list as the macOS actionBar sort menu.
    private func applySortCaps(_ caps: [String]) {
        sortCaps = caps
        var options: [(String, String)] = [("Default", "")]
        let known: [(cap: String, label: String, criteria: String)] = [
            ("dc:title", "Title", "+dc:title"),
            ("upnp:artist", "Artist", "+upnp:artist"),
            ("upnp:album", "Album", "+upnp:album"),
            ("upnp:originalTrackNumber", "Track #", "+upnp:originalTrackNumber"),
        ]
        for entry in known where caps.isEmpty || caps.contains(entry.cap) {
            options.append((entry.label, entry.criteria))
        }
        sortOptions = options
        updatingSort = true
        let list = cp_string_list_new()
        for option in sortOptions { cp_string_list_append(list, "Sort: " + option.label) }
        cp_drop_down_set_model(sortDropdown, list)
        cp_drop_down_set_selected(sortDropdown, 0)
        updatingSort = false
        gtk_widget_set_visible(sortDropdown, sortOptions.count > 1 && server != nil ? 1 : 0)
    }

    private func sortSelected() {
        guard !updatingSort else { return }
        let index = Int(cp_drop_down_get_selected(sortDropdown))
        guard sortOptions.indices.contains(index) else { return }
        sortCriteria = sortOptions[index].criteria
        reload()
    }

    private func descend(into container: MediaObject) {
        if !searchQuery.isEmpty {
            // A folder in search results opens under the server root.
            clearSearchField()
            path = [path.first ?? (id: "0", title: server?.friendlyName ?? "")]
        }
        path.append((id: container.id, title: container.title))
        reload()
    }

    private func goBack() {
        if !searchQuery.isEmpty {
            clearSearchField()
            reload()
        } else if path.count > 1 {
            path.removeLast()
            reload()
        } else {
            showServers()
        }
    }

    private func clearSearchField() {
        updatingSearch = true
        cp_editable_set_text(searchEntry, "")
        updatingSearch = false
        searchQuery = ""
    }

    private func searchChanged() {
        guard !updatingSearch, server != nil else { return }
        let text = String(cString: cp_editable_get_text(searchEntry))
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard text != searchQuery else { return }
        searchQuery = text
        reload()
    }

    private func addCurrentFolder() {
        guard let server = server, let current = path.last, searchQuery.isEmpty else { return }
        setInfo("Adding folder...")
        Task { [weak self] in
            let added = await self?.model.addContainerToPlaylist(
                server: server, objectID: current.id, sortCriteria: self?.sortCriteria ?? "") ?? 0
            DispatchQueue.main.async { self?.setInfo("Added \(added) tracks") }
        }
    }

    // ponytail: loads at most 1000 objects per folder/search; add paging on
    // scroll if that ever bites.
    private func reload() {
        guard let server = server else { return }
        loadGeneration += 1
        let generation = loadGeneration
        let query = searchQuery
        let objectID = path.last?.id ?? "0"
        let sort = sortCriteria
        setHeader(path.map { $0.title }.joined(separator: " > "))
        setInfo(query.isEmpty ? "Loading..." : "Searching...")
        gtk_widget_set_visible(sortDropdown, query.isEmpty && sortOptions.count > 1 ? 1 : 0)
        cp_list_box_remove_all(listBox)
        objects = []

        Task { [weak self] in
            guard let self = self else { return }
            do {
                var all: [MediaObject] = []
                var total = Int.max
                while all.count < total && all.count < 1000 {
                    let page: PlaybackController.BrowsePage
                    if query.isEmpty {
                        page = try await self.model.controller.browse(
                            server: server, objectID: objectID,
                            startingIndex: all.count, sortCriteria: sort)
                    } else {
                        page = try await self.model.controller.search(
                            server: server, query: query, startingIndex: all.count)
                    }
                    if page.objects.isEmpty { break }
                    all.append(contentsOf: page.objects)
                    total = page.totalMatches
                }
                let result = all
                DispatchQueue.main.async {
                    guard generation == self.loadGeneration else { return }
                    self.objects = result
                    self.rebuildList()
                }
            } catch {
                DispatchQueue.main.async {
                    guard generation == self.loadGeneration else { return }
                    self.setInfo("Error: \(error)")
                }
            }
        }
    }

    private func rebuildList() {
        cp_list_box_remove_all(listBox)
        for object in objects {
            cp_list_box_append_label(listBox, rowText(for: object))
        }
        if searchQuery.isEmpty {
            setInfo(objects.isEmpty ? "Empty folder"
                : "\(objects.count) items. Click a track to add it to the queue.")
        } else {
            setInfo("\(objects.count) results for \"\(searchQuery)\"")
        }
    }

    private func rowText(for object: MediaObject) -> String {
        if object.isContainer {
            let count = object.childCount.map { "  (\($0))" } ?? ""
            return object.title + "/" + count
        }
        var text = object.title
        if let artist = object.artist, !artist.isEmpty { text += "  -  " + artist }
        if let duration = object.duration { text += "  (" + TimeFormatter.formatTime(duration) + ")" }
        return text
    }
}
