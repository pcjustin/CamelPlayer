import CGtk4
import CamelPlayerCore
import Foundation

/// Main window, mirroring the macOS ContentView: section switcher on top,
/// section content, then mini now playing + transport + seek + volume, and a
/// settings bar at the bottom.
final class PlayerApp {
    private let model: PlayerModel

    private var window: Widget

    // All GTK objects are created in activate(), after GTK is initialized.
    private var sectionStack: Widget
    private var albumsPane: AlbumsPane!
    private var favoritesPane: FavoritesPane!
    private var recentPane: RecentPane!
    private var browsePane: BrowsePane!
    private var queuePane: QueuePane!

    // Bottom bar
    private var miniCover: CoverView!
    private var miniTitleLabel: Widget
    private var miniAlbumLabel: Widget
    private var shuffleButton: Widget
    private var prevButton: Widget
    private var playButton: Widget
    private var nextButton: Widget
    private var stopButton: Widget
    private var loopButton: Widget
    private var seekScale: Widget
    private var positionLabel: Widget
    private var durationLabel: Widget
    private var volumeIcon: Widget
    private var volumeScale: Widget
    private var volumePercentLabel: Widget

    // Settings bar
    private var deviceDropdown: Widget

    private var devices: [OutputDevice] = []
    private var deviceKey = ""
    private var updatingDevices = false
    private var updatingShuffle = false
    private var updatingVolume = false
    private var suppressSeekUntil = Date.distantPast
    private var miniKey = ""

    init(model: PlayerModel) {
        self.model = model
        window = nil
        sectionStack = nil
        miniTitleLabel = nil
        miniAlbumLabel = nil
        shuffleButton = nil
        prevButton = nil
        playButton = nil
        nextButton = nil
        stopButton = nil
        loopButton = nil
        seekScale = nil
        positionLabel = nil
        durationLabel = nil
        volumeIcon = nil
        volumeScale = nil
        volumePercentLabel = nil
        deviceDropdown = nil
    }

    func activate(_ app: UnsafeMutableRawPointer?) {
        sectionStack = cp_stack_new()
        albumsPane = AlbumsPane(model: model)
        favoritesPane = FavoritesPane(model: model)
        recentPane = RecentPane(model: model)
        browsePane = BrowsePane(model: model)
        queuePane = QueuePane(model: model)
        queuePane.onSavePlaylist = { [weak self] in self?.savePlaylist() }
        queuePane.onLoadPlaylist = { [weak self] in self?.loadPlaylist() }
        miniCover = CoverView(size: 48)
        miniTitleLabel = makeLabel("Not Playing")
        miniAlbumLabel = makeLabel("", dim: true)
        shuffleButton = cp_toggle_new_icon("media-playlist-shuffle-symbolic")
        prevButton = gtk_button_new_from_icon_name("media-skip-backward-symbolic")
        playButton = gtk_button_new_from_icon_name("media-playback-start-symbolic")
        nextButton = gtk_button_new_from_icon_name("media-skip-forward-symbolic")
        stopButton = gtk_button_new_from_icon_name("media-playback-stop-symbolic")
        loopButton = gtk_button_new_from_icon_name("media-playlist-repeat-symbolic")
        seekScale = cp_scale_new(0, 1, 1)
        positionLabel = gtk_label_new("0:00")
        durationLabel = gtk_label_new("0:00")
        volumeIcon = gtk_image_new_from_icon_name("audio-volume-high-symbolic")
        volumeScale = cp_scale_new(0, 1, 0.01)
        volumePercentLabel = gtk_label_new("100%")
        deviceDropdown = cp_drop_down_new()

        window = cp_app_window_new(app)
        cp_window_set_title(window, "CamelPlayer")
        cp_window_set_default_size(window, 920, 700)

        let root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0)

        // Section switcher
        cp_stack_add(sectionStack, albumsPane.root, "albums", "Albums")
        cp_stack_add(sectionStack, favoritesPane.root, "favorites", "Favorites")
        cp_stack_add(sectionStack, recentPane.root, "recent", "Recent")
        cp_stack_add(sectionStack, browsePane.root, "browse", "Browse")
        cp_stack_add(sectionStack, queuePane.root, "queue", "Queue")
        gtk_widget_set_vexpand(sectionStack, 1)

        let switcher = cp_stack_switcher_new(sectionStack)
        gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER)
        gtk_widget_set_margin_top(switcher, 8)
        gtk_widget_set_margin_bottom(switcher, 8)
        cp_box_append(root, switcher)
        cp_box_append(root, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL))
        cp_box_append(root, sectionStack)
        cp_box_append(root, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL))
        cp_box_append(root, makeBottomBar())
        cp_box_append(root, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL))
        cp_box_append(root, makeSettingsBar())

        cp_window_set_child(window, root)

        model.onError = { [weak self] message in
            guard let self = self else { return }
            cp_alert(self.window, message)
        }
        model.controller.onUPnPDevicesChanged = { [weak self] in self?.refreshDevices() }
        model.controller.onUPnPServersChanged = { [weak self] in
            guard let self = self else { return }
            self.model.refreshMediaServers()
            self.browsePane.serversChanged()
            self.albumsPane.refreshIfNeeded()
        }
        connect(sectionStack, "notify::visible-child", notify: true) { [weak self] in
            self?.sectionChanged()
        }

        refreshDevices()
        albumsPane.refreshIfNeeded()

        // GTK owns the main loop; pump the Foundation run loop (timers and the
        // main dispatch queue) from a GLib tick and poll playback state, the
        // same 100ms cadence as the macOS ViewModel.
        let tick: @convention(c) (UnsafeMutableRawPointer?) -> gboolean = { data in
            Unmanaged<PlayerApp>.fromOpaque(data!).takeUnretainedValue().tick()
            return 1
        }
        _ = g_timeout_add(100, tick, Unmanaged.passUnretained(self).toOpaque())

        cp_window_present(window)
    }

    // MARK: - Bottom bar (mini now playing + transport + seek + volume)

    private func makeBottomBar() -> Widget {
        let bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20)
        gtk_widget_set_margin_top(bar, 10)
        gtk_widget_set_margin_bottom(bar, 10)
        gtk_widget_set_margin_start(bar, 12)
        gtk_widget_set_margin_end(bar, 12)

        // Mini now playing
        let mini = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10)
        gtk_widget_set_size_request(mini, 200, -1)
        cp_box_append(mini, miniCover.widget)
        let miniText = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2)
        gtk_widget_set_valign(miniText, GTK_ALIGN_CENTER)
        cp_box_append(miniText, miniTitleLabel)
        cp_box_append(miniText, miniAlbumLabel)
        cp_box_append(mini, miniText)
        cp_box_append(bar, mini)

        // Transport
        let transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6)
        gtk_widget_set_valign(transport, GTK_ALIGN_CENTER)
        for button in [shuffleButton, prevButton, playButton, nextButton, stopButton, loopButton] {
            cp_button_set_has_frame(button, 0)
        }
        gtk_widget_set_tooltip_text(shuffleButton, "Shuffle")
        gtk_widget_set_tooltip_text(loopButton, "Loop: off / all / one")
        cp_box_append(transport, shuffleButton)
        cp_box_append(transport, prevButton)
        cp_box_append(transport, playButton)
        cp_box_append(transport, nextButton)
        cp_box_append(transport, stopButton)
        cp_box_append(transport, loopButton)
        cp_box_append(bar, transport)

        // Seek bar with time labels
        let seek = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2)
        gtk_widget_set_hexpand(seek, 1)
        gtk_widget_set_valign(seek, GTK_ALIGN_CENTER)
        cp_box_append(seek, seekScale)
        let times = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)
        gtk_widget_set_hexpand(positionLabel, 1)
        cp_label_set_xalign(positionLabel, 0)
        cp_label_set_xalign(durationLabel, 1)
        cp_box_append(times, positionLabel)
        cp_box_append(times, durationLabel)
        cp_box_append(seek, times)
        cp_box_append(bar, seek)

        // Volume
        let volume = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        gtk_widget_set_valign(volume, GTK_ALIGN_CENTER)
        cp_box_append(volume, volumeIcon)
        gtk_widget_set_size_request(volumeScale, 110, -1)
        cp_range_set_value(volumeScale, Double(model.volume))
        cp_box_append(volume, volumeScale)
        cp_box_append(volume, volumePercentLabel)
        cp_box_append(bar, volume)

        connect(shuffleButton, "clicked") { [weak self] in
            guard let self = self, !self.updatingShuffle else { return }
            self.model.setShuffle(cp_toggle_get_active(self.shuffleButton) != 0)
        }
        connect(prevButton, "clicked") { [weak self] in self?.model.previous() }
        connect(playButton, "clicked") { [weak self] in self?.model.togglePlayPause() }
        connect(nextButton, "clicked") { [weak self] in self?.model.next() }
        connect(stopButton, "clicked") { [weak self] in self?.model.stop() }
        connect(loopButton, "clicked") { [weak self] in
            guard let self = self else { return }
            self.model.cycleLoop()
            self.updateLoopIcon()
        }
        connectChangeValue(seekScale) { [weak self] value in
            guard let self = self else { return }
            self.suppressSeekUntil = Date().addingTimeInterval(0.5)
            self.model.seek(to: value)
        }
        connect(volumeScale, "value-changed") { [weak self] in
            guard let self = self, !self.updatingVolume else { return }
            self.model.setVolume(Float(cp_range_get_value(self.volumeScale)))
        }

        return bar
    }

    // MARK: - Settings bar (device picker + add files / folder)

    private func makeSettingsBar() -> Widget {
        let bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        gtk_widget_set_margin_top(bar, 8)
        gtk_widget_set_margin_bottom(bar, 8)
        gtk_widget_set_margin_start(bar, 12)
        gtk_widget_set_margin_end(bar, 12)

        cp_box_append(bar, makeLabel("Device:", dim: true))
        gtk_widget_set_size_request(deviceDropdown, 250, -1)
        cp_box_append(bar, deviceDropdown)
        let refreshButton = gtk_button_new_from_icon_name("view-refresh-symbolic")
        cp_button_set_has_frame(refreshButton, 0)
        gtk_widget_set_tooltip_text(refreshButton, "Refresh network devices")
        cp_box_append(bar, refreshButton)

        let spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)
        gtk_widget_set_hexpand(spacer, 1)
        cp_box_append(bar, spacer)

        let addFilesButton = gtk_button_new_with_label("Add Files")
        let addFolderButton = gtk_button_new_with_label("Add Folder")
        cp_box_append(bar, addFilesButton)
        cp_box_append(bar, addFolderButton)

        connect(refreshButton, "clicked") { [weak self] in
            self?.model.controller.refreshUPnPDevices()
            self?.refreshDevices()
        }
        connect(deviceDropdown, "notify::selected", notify: true) { [weak self] in
            self?.deviceSelected()
        }
        connect(addFilesButton, "clicked") { [weak self] in self?.openFiles() }
        connect(addFolderButton, "clicked") { [weak self] in self?.openFolder() }

        return bar
    }

    // MARK: - Tick

    private var snapshotTicks = 0

    private func tick() {
        RunLoop.main.run(until: Date())
        model.updateState()

        // Debug: CAMEL_SNAPSHOT=<path> saves a window render after 5 seconds.
        if let path = ProcessInfo.processInfo.environment["CAMEL_SNAPSHOT"] {
            snapshotTicks += 1
            if snapshotTicks == 50 {
                _ = cp_snapshot_widget_to_png(window, path)
            }
        }

        // Transport buttons
        cp_button_set_icon_name(playButton, model.isPlaying
            ? "media-playback-pause-symbolic" : "media-playback-start-symbolic")
        gtk_widget_set_sensitive(playButton,
            model.playlistItems.isEmpty || model.currentTrackNeedsRenderer ? 0 : 1)
        gtk_widget_set_sensitive(prevButton, model.canGoPrevious ? 1 : 0)
        gtk_widget_set_sensitive(nextButton, model.canGoNext ? 1 : 0)
        gtk_widget_set_sensitive(stopButton, model.isStopped ? 0 : 1)
        if (cp_toggle_get_active(shuffleButton) != 0) != model.shuffle {
            updatingShuffle = true
            cp_toggle_set_active(shuffleButton, model.shuffle ? 1 : 0)
            updatingShuffle = false
        }
        updateLoopIcon()

        // Seek + time labels
        if Date() >= suppressSeekUntil {
            if let duration = model.duration, duration > 0 {
                cp_range_set_range(seekScale, 0, duration)
                cp_range_set_value(seekScale, min(model.currentTime, duration))
                gtk_widget_set_sensitive(seekScale, 1)
            } else {
                cp_range_set_range(seekScale, 0, 1)
                cp_range_set_value(seekScale, 0)
                gtk_widget_set_sensitive(seekScale, 0)
            }
        }
        cp_label_set_text(positionLabel, TimeFormatter.formatTime(model.currentTime))
        cp_label_set_text(durationLabel, TimeFormatter.formatTime(model.duration ?? 0))

        // Volume
        let volume = model.volume
        if abs(Float(cp_range_get_value(volumeScale)) - volume) > 0.001 {
            updatingVolume = true
            cp_range_set_value(volumeScale, Double(volume))
            updatingVolume = false
        }
        cp_label_set_text(volumePercentLabel, "\(Int(volume * 100))%")
        cp_image_set_icon(volumeIcon, volumeIconName(volume))

        // Mini now playing
        let miniNow = [
            model.currentItem?.title ?? "",
            model.currentAlbum ?? "",
            model.currentCoverURL?.absoluteString ?? "",
        ].joined(separator: "|")
        if miniNow != miniKey {
            miniKey = miniNow
            cp_label_set_markup(miniTitleLabel,
                markupEscape(model.currentItem?.title ?? "Not Playing"))
            cp_label_set_markup(miniAlbumLabel,
                "<span alpha='55%'>" + markupEscape(model.currentAlbum ?? "") + "</span>")
            miniCover.setURL(model.currentCoverURL)
        }

        queuePane.refresh()
        refreshDeviceListIfChanged()
    }

    private func volumeIconName(_ volume: Float) -> String {
        if volume == 0 { return "audio-volume-muted-symbolic" }
        if volume < 0.33 { return "audio-volume-low-symbolic" }
        if volume < 0.66 { return "audio-volume-medium-symbolic" }
        return "audio-volume-high-symbolic"
    }

    private func updateLoopIcon() {
        // No distinct repeat-one icon guaranteed in the theme; mark it via tooltip
        // and a CSS accent class instead.
        switch model.loopMode {
        case .off:
            cp_button_set_icon_name(loopButton, "media-playlist-repeat-symbolic")
            gtk_widget_remove_css_class(loopButton, "accent")
        case .all:
            cp_button_set_icon_name(loopButton, "media-playlist-repeat-symbolic")
            gtk_widget_add_css_class(loopButton, "accent")
        case .one:
            cp_button_set_icon_name(loopButton, "media-playlist-repeat-song-symbolic")
            gtk_widget_add_css_class(loopButton, "accent")
        }
    }

    private func sectionChanged() {
        guard let name = cp_stack_visible_name(sectionStack).map({ String(cString: $0) }) else { return }
        switch name {
        case "albums": albumsPane.refreshIfNeeded()
        case "favorites": favoritesPane.showList()
        case "recent": recentPane.showList()
        default: break
        }
    }

    // MARK: - Devices

    private func refreshDevices() {
        model.refreshDevices()
        refreshDeviceListIfChanged()
    }

    private func refreshDeviceListIfChanged() {
        let list = model.outputDevices
        let key = list.map { $0.id }.joined(separator: "|") + "|" + (model.currentOutputDevice?.id ?? "")
        guard key != deviceKey else { return }
        deviceKey = key
        devices = list
        updatingDevices = true
        let strings = cp_string_list_new()
        for device in devices {
            let prefix: String
            if case .local = device.type { prefix = "Local: " } else { prefix = "Network: " }
            cp_string_list_append(strings, prefix + device.name)
        }
        cp_drop_down_set_model(deviceDropdown, strings)
        if let index = devices.firstIndex(where: { $0.id == model.currentOutputDevice?.id }) {
            cp_drop_down_set_selected(deviceDropdown, UInt32(index))
        }
        updatingDevices = false
    }

    private func deviceSelected() {
        guard !updatingDevices else { return }
        let index = Int(cp_drop_down_get_selected(deviceDropdown))
        guard devices.indices.contains(index) else { return }
        model.setOutputDevice(devices[index])
    }

    // MARK: - File dialogs

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
            Unmanaged<PlayerApp>.fromOpaque(data!).takeUnretainedValue().model.addFiles(urls)
        }
        cp_file_dialog_open_multiple(window, callback, Unmanaged.passUnretained(self).toOpaque())
    }

    private func openFolder() {
        let callback: GAsyncReadyCallback = { source, result, data in
            guard let path = cp_file_dialog_folder_finish(source, result) else { return }
            let url = URL(fileURLWithPath: String(cString: path))
            g_free(path)
            Unmanaged<PlayerApp>.fromOpaque(data!).takeUnretainedValue()
                .model.addFiles(scanFolder(url))
        }
        cp_file_dialog_select_folder(window, callback, Unmanaged.passUnretained(self).toOpaque())
    }

    fileprivate func savePlaylist() {
        let callback: GAsyncReadyCallback = { source, result, data in
            guard let path = cp_file_dialog_save_finish(source, result) else { return }
            let url = URL(fileURLWithPath: String(cString: path))
            g_free(path)
            Unmanaged<PlayerApp>.fromOpaque(data!).takeUnretainedValue()
                .model.exportPlaylist(to: url)
        }
        cp_file_dialog_save(window, "playlist.json", callback, Unmanaged.passUnretained(self).toOpaque())
    }

    fileprivate func loadPlaylist() {
        let callback: GAsyncReadyCallback = { source, result, data in
            guard let path = cp_file_dialog_open_finish(source, result) else { return }
            let url = URL(fileURLWithPath: String(cString: path))
            g_free(path)
            Unmanaged<PlayerApp>.fromOpaque(data!).takeUnretainedValue()
                .model.importPlaylist(from: url)
        }
        cp_file_dialog_open(window, callback, Unmanaged.passUnretained(self).toOpaque())
    }

}

let model = try PlayerModel()
let playerApp = PlayerApp(model: model)
let app = cp_application_new("com.camelplayer.gtk")
connectRaw(app, "activate") { playerApp.activate(app) }
exit(cp_application_run(app))
