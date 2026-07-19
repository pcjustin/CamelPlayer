import CGtk4
import CamelPlayerCore
import Foundation

private final class SignalBox {
    let handler: () -> Void
    init(_ handler: @escaping () -> Void) { self.handler = handler }
}

/* notify:: signals pass an extra GParamSpec argument, so they need their own
   callback shape. */
private func connectRaw(_ instance: UnsafeMutableRawPointer?, _ signal: String,
                        notify: Bool = false, _ handler: @escaping () -> Void) {
    let box = Unmanaged.passRetained(SignalBox(handler)).toOpaque()
    let plain: @convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void = { _, data in
        Unmanaged<SignalBox>.fromOpaque(data!).takeUnretainedValue().handler()
    }
    let notifying: @convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void = { _, _, data in
        Unmanaged<SignalBox>.fromOpaque(data!).takeUnretainedValue().handler()
    }
    let drop: @convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void = { data, _ in
        Unmanaged<SignalBox>.fromOpaque(data!).release()
    }
    let callback = notify ? unsafeBitCast(notifying, to: GCallback.self)
                          : unsafeBitCast(plain, to: GCallback.self)
    _ = cp_signal_connect(instance, signal, callback, box,
                          unsafeBitCast(drop, to: GClosureNotify.self))
}

private func connect(_ widget: UnsafeMutablePointer<GtkWidget>?, _ signal: String,
                     notify: Bool = false, _ handler: @escaping () -> Void) {
    connectRaw(UnsafeMutableRawPointer(widget), signal, notify: notify, handler)
}

private final class ValueBox {
    let handler: (Double) -> Void
    init(_ handler: @escaping (Double) -> Void) { self.handler = handler }
}

/* change-value carries (GtkScrollType, double) and fires only on user
   interaction, so programmatic set_value cannot loop back into a seek. */
private func connectChangeValue(_ widget: UnsafeMutablePointer<GtkWidget>?,
                                _ handler: @escaping (Double) -> Void) {
    let box = Unmanaged.passRetained(ValueBox(handler)).toOpaque()
    let call: @convention(c) (UnsafeMutableRawPointer?, UInt32, Double, UnsafeMutableRawPointer?) -> gboolean = { _, _, value, data in
        Unmanaged<ValueBox>.fromOpaque(data!).takeUnretainedValue().handler(value)
        return 0
    }
    let drop: @convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void = { data, _ in
        Unmanaged<ValueBox>.fromOpaque(data!).release()
    }
    _ = cp_signal_connect(UnsafeMutableRawPointer(widget), "change-value",
                          unsafeBitCast(call, to: GCallback.self), box,
                          unsafeBitCast(drop, to: GClosureNotify.self))
}

private final class RowBox {
    let handler: (Int) -> Void
    init(_ handler: @escaping (Int) -> Void) { self.handler = handler }
}

private func connectRowActivated(_ widget: UnsafeMutablePointer<GtkWidget>?,
                                 _ handler: @escaping (Int) -> Void) {
    let box = Unmanaged.passRetained(RowBox(handler)).toOpaque()
    let call: @convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void = { _, row, data in
        Unmanaged<RowBox>.fromOpaque(data!).takeUnretainedValue()
            .handler(Int(cp_list_box_row_index(row)))
    }
    let drop: @convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void = { data, _ in
        Unmanaged<RowBox>.fromOpaque(data!).release()
    }
    _ = cp_signal_connect(UnsafeMutableRawPointer(widget), "row-activated",
                          unsafeBitCast(call, to: GCallback.self), box,
                          unsafeBitCast(drop, to: GClosureNotify.self))
}

/// UPnP media server browser: server list -> folder navigation -> tracks.
/// Mirrors the macOS BrowseView minus album art and sort options.
final class BrowsePane {
    private let controller: PlaybackController
    private(set) var root: UnsafeMutablePointer<GtkWidget>?

    private var headerLabel: UnsafeMutablePointer<GtkWidget>?
    private var searchEntry: UnsafeMutablePointer<GtkWidget>?
    private var infoLabel: UnsafeMutablePointer<GtkWidget>?
    private var listBox: UnsafeMutablePointer<GtkWidget>?

    private var server: UPnPDevice?
    private var path: [(id: String, title: String)] = []
    private var objects: [MediaObject] = []
    private var serverRows: [UPnPDevice] = []
    private var searchQuery = ""
    private var updatingSearch = false
    /// Discards results of superseded loads.
    private var loadGeneration = 0

    private var showingServers: Bool { server == nil }

    init(controller: PlaybackController) {
        self.controller = controller

        root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)
        gtk_widget_set_margin_top(root, 12)
        gtk_widget_set_margin_bottom(root, 12)
        gtk_widget_set_margin_start(root, 12)
        gtk_widget_set_margin_end(root, 12)

        let header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        let backButton = gtk_button_new_with_label("Back")
        headerLabel = gtk_label_new("Network Servers")
        cp_label_ellipsize_end(headerLabel)
        gtk_widget_set_hexpand(headerLabel, 1)
        let addFolderButton = gtk_button_new_with_label("Add Folder")
        cp_box_append(header, backButton)
        cp_box_append(header, headerLabel)
        cp_box_append(header, addFolderButton)
        cp_box_append(root, header)

        searchEntry = gtk_search_entry_new()
        cp_box_append(root, searchEntry)

        infoLabel = gtk_label_new("")
        cp_label_ellipsize_end(infoLabel)
        cp_box_append(root, infoLabel)

        listBox = gtk_list_box_new()
        let scroller = cp_scrolled_window(listBox)
        gtk_widget_set_vexpand(scroller, 1)
        cp_box_append(root, scroller)

        connect(backButton, "clicked") { [weak self] in self?.goBack() }
        connect(addFolderButton, "clicked") { [weak self] in self?.addCurrentFolder() }
        connect(searchEntry, "search-changed") { [weak self] in self?.searchChanged() }
        connectRowActivated(listBox) { [weak self] index in self?.rowActivated(index) }

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
        cp_label_set_text(headerLabel, text)
    }

    private func showServers() {
        server = nil
        path = []
        objects = []
        loadGeneration += 1
        setHeader("Network Servers")
        serverRows = controller.availableMediaServers
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
        } else if controller.addTrackToPlaylist(object) {
            setInfo("Added: \(object.title)")
        }
    }

    private func open(_ device: UPnPDevice) {
        server = device
        path = [(id: "0", title: device.friendlyName)]
        clearSearchField()
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
            let added = (try? await self?.controller.addContainerToPlaylist(
                server: server, objectID: current.id)) ?? 0
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
        setHeader(path.map { $0.title }.joined(separator: " > "))
        setInfo(query.isEmpty ? "Loading..." : "Searching...")
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
                        page = try await self.controller.browse(
                            server: server, objectID: objectID, startingIndex: all.count)
                    } else {
                        page = try await self.controller.search(
                            server: server, query: query, startingIndex: all.count)
                    }
                    if page.objects.isEmpty { break }
                    all.append(contentsOf: page.objects)
                    total = page.totalMatches
                }
                let result = all
                let totalMatches = total
                DispatchQueue.main.async {
                    guard generation == self.loadGeneration else { return }
                    self.objects = result
                    self.rebuildList(totalMatches: totalMatches)
                }
            } catch {
                DispatchQueue.main.async {
                    guard generation == self.loadGeneration else { return }
                    self.setInfo("Error: \(error)")
                }
            }
        }
    }

    private func rebuildList(totalMatches: Int) {
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

final class PlayerApp {
    private let controller: PlaybackController

    private var window: UnsafeMutablePointer<GtkWidget>?
    private var titleLabel: UnsafeMutablePointer<GtkWidget>?
    private var statusLabel: UnsafeMutablePointer<GtkWidget>?
    private var playButton: UnsafeMutablePointer<GtkWidget>?
    private var deviceDropdown: UnsafeMutablePointer<GtkWidget>?
    private var seekScale: UnsafeMutablePointer<GtkWidget>?
    private var volumeScale: UnsafeMutablePointer<GtkWidget>?
    private var playlistBox: UnsafeMutablePointer<GtkWidget>?

    private var browsePane: BrowsePane?
    private var devices: [OutputDevice] = []
    private var updatingDevices = false
    private var lastPlaylistCount = -1
    private var lastPosition = -1
    /// While the user drags the seek bar, tick updates would fight the drag.
    private var suppressSeekUntil = Date.distantPast

    init(controller: PlaybackController) {
        self.controller = controller
    }

    func activate(_ app: UnsafeMutableRawPointer?) {
        window = cp_app_window_new(app)
        cp_window_set_title(window, "CamelPlayer")
        cp_window_set_default_size(window, 480, 600)

        let root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12)
        gtk_widget_set_margin_top(root, 16)
        gtk_widget_set_margin_bottom(root, 16)
        gtk_widget_set_margin_start(root, 16)
        gtk_widget_set_margin_end(root, 16)

        titleLabel = gtk_label_new("No track")
        statusLabel = gtk_label_new("Stopped")
        cp_box_append(root, titleLabel)
        cp_box_append(root, statusLabel)

        seekScale = cp_scale_new(0, 1, 1)
        cp_box_append(root, seekScale)

        let transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        gtk_widget_set_halign(transport, GTK_ALIGN_CENTER)
        let prevButton = gtk_button_new_with_label("Prev")
        playButton = gtk_button_new_with_label("Play")
        let stopButton = gtk_button_new_with_label("Stop")
        let nextButton = gtk_button_new_with_label("Next")
        cp_box_append(transport, prevButton)
        cp_box_append(transport, playButton)
        cp_box_append(transport, stopButton)
        cp_box_append(transport, nextButton)
        cp_box_append(root, transport)

        playlistBox = gtk_list_box_new()
        let scroller = cp_scrolled_window(playlistBox)
        gtk_widget_set_vexpand(scroller, 1)
        cp_box_append(root, scroller)

        let volumeRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        cp_box_append(volumeRow, gtk_label_new("Volume"))
        volumeScale = cp_scale_new(0, 1, 0.01)
        gtk_widget_set_hexpand(volumeScale, 1)
        cp_range_set_value(volumeScale, Double(controller.volume))
        cp_box_append(volumeRow, volumeScale)
        cp_box_append(root, volumeRow)

        let bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        let addButton = gtk_button_new_with_label("Add Files")
        deviceDropdown = cp_drop_down_new()
        gtk_widget_set_hexpand(deviceDropdown, 1)
        let refreshButton = gtk_button_new_with_label("Refresh")
        cp_box_append(bottom, addButton)
        cp_box_append(bottom, deviceDropdown)
        cp_box_append(bottom, refreshButton)
        cp_box_append(root, bottom)

        let notebook = cp_notebook_new()
        cp_notebook_append(notebook, root, "Player")
        let browse = BrowsePane(controller: controller)
        browsePane = browse
        cp_notebook_append(notebook, browse.root, "Browse")
        cp_window_set_child(window, notebook)

        connect(prevButton, "clicked") { [weak self] in
            guard let self else { return }
            Task { try? await self.controller.previous() }
        }
        connect(playButton, "clicked") { [weak self] in self?.playPause() }
        connect(stopButton, "clicked") { [weak self] in self?.controller.stop() }
        connect(nextButton, "clicked") { [weak self] in
            guard let self else { return }
            Task { try? await self.controller.next() }
        }
        connect(addButton, "clicked") { [weak self] in self?.openFiles() }
        connect(refreshButton, "clicked") { [weak self] in
            self?.controller.refreshUPnPDevices()
            self?.refreshDevices()
        }
        connect(deviceDropdown, "notify::selected", notify: true) { [weak self] in
            self?.deviceSelected()
        }
        connectChangeValue(seekScale) { [weak self] value in
            guard let self else { return }
            self.suppressSeekUntil = Date().addingTimeInterval(0.5)
            Task { try? await self.controller.seek(to: value) }
        }
        connect(volumeScale, "value-changed") { [weak self] in
            guard let self else { return }
            self.controller.volume = Float(cp_range_get_value(self.volumeScale))
        }
        connectRowActivated(playlistBox) { [weak self] index in
            guard let self else { return }
            Task { try? await self.controller.playItem(at: index) }
        }

        controller.onUPnPDevicesChanged = { [weak self] in self?.refreshDevices() }
        controller.onUPnPServersChanged = { [weak self] in self?.browsePane?.serversChanged() }
        refreshDevices()

        // GTK owns the main loop; pump the Foundation run loop (timers and the
        // main dispatch queue) from a GLib tick, and poll playback state the
        // same way the macOS GUI does.
        let tick: @convention(c) (UnsafeMutableRawPointer?) -> gboolean = { data in
            Unmanaged<PlayerApp>.fromOpaque(data!).takeUnretainedValue().tick()
            return 1
        }
        _ = g_timeout_add(100, tick, Unmanaged.passUnretained(self).toOpaque())

        cp_window_present(window)
    }

    private func tick() {
        RunLoop.main.run(until: Date())

        cp_label_set_text(titleLabel, controller.currentItem?.title ?? "No track")

        let state = controller.currentState
        var status: String
        switch state {
        case .playing: status = "Playing"
        case .paused: status = "Paused"
        case .stopped: status = "Stopped"
        }
        if state != .stopped {
            status += "  " + TimeFormatter.formatTime(controller.currentTime)
            if let duration = controller.duration {
                status += " / " + TimeFormatter.formatTime(duration)
            }
            if let format = controller.getFileFormat() {
                status += "  [" + format + "]"
            }
        }
        cp_label_set_text(statusLabel, status)
        cp_button_set_label(playButton, state == .playing ? "Pause" : "Play")

        if Date() >= suppressSeekUntil {
            if let duration = controller.duration, duration > 0 {
                cp_range_set_range(seekScale, 0, duration)
                cp_range_set_value(seekScale, min(controller.currentTime, duration))
            } else {
                cp_range_set_range(seekScale, 0, 1)
                cp_range_set_value(seekScale, 0)
            }
        }

        let count = controller.getPlaylistCount()
        if count != lastPlaylistCount {
            lastPlaylistCount = count
            cp_list_box_remove_all(playlistBox)
            for item in controller.getPlaylistItems() {
                cp_list_box_append_label(playlistBox, item.title)
            }
            lastPosition = -1
        }
        let position = controller.getCurrentPosition()
        if position != lastPosition {
            lastPosition = position
            cp_list_box_select_index(playlistBox, Int32(position))
        }
    }

    private func playPause() {
        if controller.currentState == .playing {
            controller.pause()
        } else {
            Task { try? await controller.play() }
        }
    }

    private func refreshDevices() {
        devices = controller.listAllOutputDevices()
        updatingDevices = true
        let list = cp_string_list_new()
        for device in devices {
            cp_string_list_append(list, device.name)
        }
        cp_drop_down_set_model(deviceDropdown, list)
        if let index = devices.firstIndex(where: { $0.id == controller.currentOutputDevice.id }) {
            cp_drop_down_set_selected(deviceDropdown, UInt32(index))
        }
        updatingDevices = false
    }

    private func deviceSelected() {
        guard !updatingDevices else { return }
        let index = Int(cp_drop_down_get_selected(deviceDropdown))
        guard devices.indices.contains(index) else { return }
        do {
            try controller.setOutputDevice(devices[index])
        } catch {
            FileHandle.standardError.write(Data("GTK: Failed to set output device: \(error)\n".utf8))
            refreshDevices()
        }
    }

    private func openFiles() {
        let callback: GAsyncReadyCallback = { source, result, data in
            guard let files = cp_file_dialog_finish(source, result) else { return }
            defer { g_object_unref(files) }
            var urls: [URL] = []
            for i in 0..<cp_list_model_n_items(files) {
                if let path = cp_file_path_at(files, i) {
                    urls.append(URL(fileURLWithPath: String(cString: path)))
                    g_free(path)
                }
            }
            Unmanaged<PlayerApp>.fromOpaque(data!).takeUnretainedValue()
                .controller.addToPlaylist(urls: urls)
        }
        cp_file_dialog_open_multiple(window, callback, Unmanaged.passUnretained(self).toOpaque())
    }
}

let controller = try PlaybackController()
let playerApp = PlayerApp(controller: controller)
let app = cp_application_new("com.camelplayer.gtk")
connectRaw(app, "activate") { playerApp.activate(app) }
exit(cp_application_run(app))
