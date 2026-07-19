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

final class PlayerApp {
    private let controller: PlaybackController

    private var window: UnsafeMutablePointer<GtkWidget>?
    private var titleLabel: UnsafeMutablePointer<GtkWidget>?
    private var statusLabel: UnsafeMutablePointer<GtkWidget>?
    private var playButton: UnsafeMutablePointer<GtkWidget>?
    private var deviceDropdown: UnsafeMutablePointer<GtkWidget>?

    private var devices: [OutputDevice] = []
    private var updatingDevices = false

    init(controller: PlaybackController) {
        self.controller = controller
    }

    func activate(_ app: UnsafeMutableRawPointer?) {
        window = cp_app_window_new(app)
        cp_window_set_title(window, "CamelPlayer")
        cp_window_set_default_size(window, 480, 220)

        let root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12)
        gtk_widget_set_margin_top(root, 16)
        gtk_widget_set_margin_bottom(root, 16)
        gtk_widget_set_margin_start(root, 16)
        gtk_widget_set_margin_end(root, 16)

        titleLabel = gtk_label_new("No track")
        statusLabel = gtk_label_new("Stopped")
        cp_box_append(root, titleLabel)
        cp_box_append(root, statusLabel)

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

        let bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)
        let addButton = gtk_button_new_with_label("Add Files")
        deviceDropdown = cp_drop_down_new()
        gtk_widget_set_hexpand(deviceDropdown, 1)
        let refreshButton = gtk_button_new_with_label("Refresh")
        cp_box_append(bottom, addButton)
        cp_box_append(bottom, deviceDropdown)
        cp_box_append(bottom, refreshButton)
        cp_box_append(root, bottom)

        cp_window_set_child(window, root)

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

        controller.onUPnPDevicesChanged = { [weak self] in self?.refreshDevices() }
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
