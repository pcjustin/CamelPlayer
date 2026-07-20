import CGtk4
import CamelPlayerCore
import Foundation

/// Queue section: now-playing panel on the left, playlist on the right,
/// mirroring the macOS queueSection (fixed left pane, no draggable divider).
final class QueuePane {
    private let model: PlayerModel
    private(set) var root: Widget

    // Now playing (left)
    private let cover = CoverView(size: 180)
    private var trackLabel: Widget
    private var albumLabel: Widget
    private var formatLabel: Widget
    private var starButton: Widget

    // Playlist (right)
    private var countLabel: Widget
    private var saveButton: Widget
    private var clearButton: Widget
    private var listBox: Widget
    private var emptyLabel: Widget
    private var listStack: Widget
    private var listPage: Widget
    private var emptyPage: Widget

    var onSavePlaylist: (() -> Void)?
    var onLoadPlaylist: (() -> Void)?

    private var renderedKey = ""
    private var nowPlayingKey = ""
    private var lastPanedPosition: Int32 = 0
    private var lastScrolledPosition = -1
    private var scrollCountdown = 0

    init(model: PlayerModel) {
        self.model = model

        // Left: now playing
        let left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12)
        gtk_widget_set_size_request(left, 200, -1)
        gtk_widget_set_margin_top(left, 16)
        gtk_widget_set_margin_bottom(left, 16)
        gtk_widget_set_margin_start(left, 16)
        gtk_widget_set_margin_end(left, 16)
        gtk_widget_set_valign(left, GTK_ALIGN_START)

        gtk_widget_set_halign(cover.widget, GTK_ALIGN_CENTER)
        cp_box_append(left, cover.widget)
        trackLabel = makeLabel("No Track Loaded", bold: true)
        cp_label_set_xalign(trackLabel, Float(0.5))
        cp_box_append(left, trackLabel)
        albumLabel = makeLabel("", dim: true)
        cp_label_set_xalign(albumLabel, Float(0.5))
        cp_box_append(left, albumLabel)
        formatLabel = gtk_label_new("")
        cp_label_ellipsize_end(formatLabel)
        cp_box_append(left, formatLabel)
        starButton = gtk_button_new_from_icon_name("non-starred-symbolic")
        cp_button_set_has_frame(starButton, 0)
        gtk_widget_set_halign(starButton, GTK_ALIGN_CENTER)
        cp_box_append(left, starButton)

        // Right: playlist
        let right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0)
        gtk_widget_set_hexpand(right, 1)

        let header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        gtk_widget_set_margin_top(header, 8)
        gtk_widget_set_margin_bottom(header, 8)
        gtk_widget_set_margin_start(header, 12)
        gtk_widget_set_margin_end(header, 12)
        let title = makeLabel("Playlist", bold: true)
        gtk_widget_set_hexpand(title, 1)
        countLabel = makeLabel("0 tracks", dim: true)
        let loadButton = gtk_button_new_from_icon_name("document-open-symbolic")
        cp_button_set_has_frame(loadButton, 0)
        gtk_widget_set_tooltip_text(loadButton, "Load Playlist")
        saveButton = gtk_button_new_from_icon_name("document-save-symbolic")
        cp_button_set_has_frame(saveButton, 0)
        gtk_widget_set_tooltip_text(saveButton, "Save Playlist")
        clearButton = gtk_button_new_from_icon_name("user-trash-symbolic")
        cp_button_set_has_frame(clearButton, 0)
        gtk_widget_set_tooltip_text(clearButton, "Clear Playlist")
        cp_box_append(header, title)
        cp_box_append(header, countLabel)
        cp_box_append(header, loadButton)
        cp_box_append(header, saveButton)
        cp_box_append(header, clearButton)
        cp_box_append(right, header)
        cp_box_append(right, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL))

        listBox = gtk_list_box_new()
        cp_list_box_single_click(listBox, 0)
        listPage = cp_scrolled_window(listBox)
        gtk_widget_set_vexpand(listPage, 1)

        emptyLabel = gtk_label_new("No tracks in playlist\nAdd files or folders to get started")
        cp_label_justify_center(emptyLabel)
        emptyPage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)
        gtk_widget_set_valign(emptyPage, GTK_ALIGN_CENTER)
        cp_box_append(emptyPage, emptyLabel)

        listStack = cp_stack_new()
        gtk_widget_set_vexpand(listStack, 1)
        cp_stack_add(listStack, listPage, "list", nil)
        cp_stack_add(listStack, emptyPage, "empty", nil)
        cp_box_append(right, listStack)

        // Draggable divider with the persisted width, same key as macOS.
        root = cp_paned_new(left, right)
        let savedWidth = UserDefaults.standard.object(forKey: "ui.leftPaneWidth") as? Double ?? 260
        lastPanedPosition = Int32(min(420, max(200, savedWidth)))
        cp_paned_set_position(root, lastPanedPosition)

        connect(starButton, "clicked") { [weak self] in
            guard let self = self, let item = self.model.currentItem else { return }
            self.model.toggleFavoriteTrack(item)
            self.refreshNowPlaying(force: true)
        }
        connect(loadButton, "clicked") { [weak self] in self?.onLoadPlaylist?() }
        connect(saveButton, "clicked") { [weak self] in self?.onSavePlaylist?() }
        connect(clearButton, "clicked") { [weak self] in self?.model.clearPlaylist() }
        connectRowActivated(listBox) { [weak self] index in self?.model.playItem(at: index) }
    }

    /// Called from the tick; updates only on visible state changes.
    func refresh() {
        refreshNowPlaying(force: false)
        refreshPlaylist()

        let position = cp_paned_get_position(root)
        if position != lastPanedPosition, position > 0 {
            lastPanedPosition = position
            UserDefaults.standard.set(Double(position), forKey: "ui.leftPaneWidth")
        }

        // Keep the playing track visible; wait a tick after rebuild so rows
        // have an allocation to scroll to.
        if model.currentPosition != lastScrolledPosition, model.currentPosition >= 0 {
            lastScrolledPosition = model.currentPosition
            scrollCountdown = 2
        }
        if scrollCountdown > 0 {
            scrollCountdown -= 1
            if scrollCountdown == 0 {
                cp_list_box_scroll_to_index(listBox, Int32(model.currentPosition))
            }
        }
    }

    private func refreshNowPlaying(force: Bool) {
        let item = model.currentItem
        let starred = item.map { model.isFavoriteTrack($0.url.absoluteString) } ?? false
        let key = [
            item?.url.absoluteString ?? "",
            model.currentAlbum ?? "",
            model.formatInfo ?? "",
            model.isBitPerfect.map(String.init) ?? "nil",
            model.currentCoverURL?.absoluteString ?? "",
            String(starred),
        ].joined(separator: "|")
        guard force || key != nowPlayingKey else { return }
        nowPlayingKey = key

        cp_label_set_markup(trackLabel, "<b>" + markupEscape(item?.title ?? "No Track Loaded") + "</b>")
        cp_label_set_markup(albumLabel, "<span alpha='55%'>" + markupEscape(model.currentAlbum ?? "") + "</span>")
        var format = markupEscape(model.formatInfo ?? "")
        if let bitPerfect = model.isBitPerfect {
            format += bitPerfect
                ? "  <span foreground='#2da44e'>bit-perfect</span>"
                : "  <span foreground='#bf8700'>resampling</span>"
        }
        cp_label_set_markup(formatLabel, format)
        cover.setURL(model.currentCoverURL)
        cp_button_set_icon_name(starButton, starred ? "starred-symbolic" : "non-starred-symbolic")
        gtk_widget_set_visible(starButton, item != nil ? 1 : 0)
    }

    private func refreshPlaylist() {
        let items = model.playlistItems
        let key = items.map { $0.url.absoluteString }.joined(separator: "|")
            + "|\(model.currentPosition)|\(model.isPlaying)"
        guard key != renderedKey else { return }
        renderedKey = key

        cp_label_set_text(countLabel, "\(items.count) tracks")
        gtk_widget_set_visible(saveButton, items.isEmpty ? 0 : 1)
        gtk_widget_set_visible(clearButton, items.isEmpty ? 0 : 1)
        guard !items.isEmpty else {
            cp_stack_set_visible(listStack, emptyPage)
            return
        }
        cp_stack_set_visible(listStack, listPage)

        cp_list_box_remove_all(listBox)
        for (index, item) in items.enumerated() {
            let isCurrent = index == model.currentPosition
            let row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
            gtk_widget_set_margin_top(row, 2)
            gtk_widget_set_margin_bottom(row, 2)

            let indicator: Widget
            if isCurrent {
                indicator = gtk_image_new_from_icon_name(
                    model.isPlaying ? "audio-volume-high-symbolic" : "media-playback-pause-symbolic")
            } else {
                indicator = makeLabel("\(index + 1)", dim: true)
                cp_label_set_xalign(indicator, Float(0.5))
            }
            gtk_widget_set_size_request(indicator, 24, -1)
            cp_box_append(row, indicator)

            let title = makeLabel(item.title, bold: isCurrent)
            gtk_widget_set_hexpand(title, 1)
            cp_box_append(row, title)

            let up = gtk_button_new_from_icon_name("go-up-symbolic")
            cp_button_set_has_frame(up, 0)
            gtk_widget_set_sensitive(up, index == 0 ? 0 : 1)
            connect(up, "clicked") { [weak self] in
                self?.model.movePlaylistItem(from: index, to: index - 1)
            }
            cp_box_append(row, up)

            let down = gtk_button_new_from_icon_name("go-down-symbolic")
            cp_button_set_has_frame(down, 0)
            gtk_widget_set_sensitive(down, index == items.count - 1 ? 0 : 1)
            connect(down, "clicked") { [weak self] in
                self?.model.movePlaylistItem(from: index, to: index + 2)
            }
            cp_box_append(row, down)

            let remove = gtk_button_new_from_icon_name("edit-delete-symbolic")
            cp_button_set_has_frame(remove, 0)
            connect(remove, "clicked") { [weak self] in
                self?.model.removeFromPlaylist(at: index)
            }
            cp_box_append(row, remove)

            cp_list_box_append(listBox, row)
        }
    }
}
