import CGtk4
import CamelPlayerCore
import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

typealias Widget = UnsafeMutablePointer<GtkWidget>?

// MARK: - Signal connection

final class SignalBox {
    let handler: () -> Void
    init(_ handler: @escaping () -> Void) { self.handler = handler }
}

/* notify:: signals pass an extra GParamSpec argument, so they need their own
   callback shape. */
func connectRaw(_ instance: UnsafeMutableRawPointer?, _ signal: String,
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

func connect(_ widget: Widget, _ signal: String,
             notify: Bool = false, _ handler: @escaping () -> Void) {
    connectRaw(UnsafeMutableRawPointer(widget), signal, notify: notify, handler)
}

final class ValueBox {
    let handler: (Double) -> Void
    init(_ handler: @escaping (Double) -> Void) { self.handler = handler }
}

/* change-value carries (GtkScrollType, double) and fires only on user
   interaction, so programmatic set_value cannot loop back into a seek. */
func connectChangeValue(_ widget: Widget, _ handler: @escaping (Double) -> Void) {
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

final class RowBox {
    let handler: (Int) -> Void
    init(_ handler: @escaping (Int) -> Void) { self.handler = handler }
}

private func connectIndexSignal(_ widget: Widget, _ signal: String,
                                index: @escaping (UnsafeMutableRawPointer?) -> Int,
                                _ handler: @escaping (Int) -> Void) {
    final class Ctx {
        let index: (UnsafeMutableRawPointer?) -> Int
        let handler: (Int) -> Void
        init(_ index: @escaping (UnsafeMutableRawPointer?) -> Int, _ handler: @escaping (Int) -> Void) {
            self.index = index
            self.handler = handler
        }
    }
    let box = Unmanaged.passRetained(Ctx(index, handler)).toOpaque()
    let call: @convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void = { _, child, data in
        let ctx = Unmanaged<Ctx>.fromOpaque(data!).takeUnretainedValue()
        ctx.handler(ctx.index(child))
    }
    let drop: @convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void = { data, _ in
        Unmanaged<Ctx>.fromOpaque(data!).release()
    }
    _ = cp_signal_connect(UnsafeMutableRawPointer(widget), signal,
                          unsafeBitCast(call, to: GCallback.self), box,
                          unsafeBitCast(drop, to: GClosureNotify.self))
}

func connectRowActivated(_ widget: Widget, _ handler: @escaping (Int) -> Void) {
    connectIndexSignal(widget, "row-activated",
                       index: { Int(cp_list_box_row_index($0)) }, handler)
}

func connectChildActivated(_ widget: Widget, _ handler: @escaping (Int) -> Void) {
    connectIndexSignal(widget, "child-activated",
                       index: { Int(cp_flow_box_child_index($0)) }, handler)
}

/// Fires when a scrolled window reaches its bottom edge (infinite scroll).
func connectBottomReached(_ widget: Widget, _ handler: @escaping () -> Void) {
    let box = Unmanaged.passRetained(SignalBox(handler)).toOpaque()
    let call: @convention(c) (UnsafeMutableRawPointer?, UInt32, UnsafeMutableRawPointer?) -> Void = { _, pos, data in
        if pos == GTK_POS_BOTTOM.rawValue {
            Unmanaged<SignalBox>.fromOpaque(data!).takeUnretainedValue().handler()
        }
    }
    let drop: @convention(c) (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) -> Void = { data, _ in
        Unmanaged<SignalBox>.fromOpaque(data!).release()
    }
    _ = cp_signal_connect(UnsafeMutableRawPointer(widget), "edge-reached",
                          unsafeBitCast(call, to: GCallback.self), box,
                          unsafeBitCast(drop, to: GClosureNotify.self))
}

// MARK: - Small widget builders

func makeLabel(_ text: String, dim: Bool = false, bold: Bool = false) -> Widget {
    let label = gtk_label_new(nil)
    var escaped = text
        .replacingOccurrences(of: "&", with: "&amp;")
        .replacingOccurrences(of: "<", with: "&lt;")
        .replacingOccurrences(of: ">", with: "&gt;")
    if bold { escaped = "<b>" + escaped + "</b>" }
    if dim { escaped = "<span alpha='55%'>" + escaped + "</span>" }
    cp_label_set_markup(label, escaped)
    cp_label_ellipsize_end(label)
    cp_label_set_xalign(label, 0)
    return label
}

func markupEscape(_ text: String) -> String {
    text.replacingOccurrences(of: "&", with: "&amp;")
        .replacingOccurrences(of: "<", with: "&lt;")
        .replacingOccurrences(of: ">", with: "&gt;")
}

// MARK: - Folder scanning (port of FilePickerHelper.scanFolder)

func scanFolder(_ url: URL) -> [URL] {
    guard let enumerator = FileManager.default.enumerator(
        at: url, includingPropertiesForKeys: nil) else { return [] }
    var files: [URL] = []
    for case let file as URL in enumerator
        where audioFileExtensions.contains(file.pathExtension.lowercased()) {
        files.append(file)
    }
    return files.sorted { $0.path < $1.path }
}

// MARK: - Cover art cache

/// Two-level cache for album art: GdkTexture in memory plus raw bytes on disk.
/// Textures are cached for the lifetime of the process.
enum CoverCache {
    private static var memory: [String: OpaquePointer] = [:]
    private static var failed: Set<String> = []

    private static let directory: URL = {
        let base = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CamelPlayer/Covers", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }()

    private static func fileURL(for url: URL) -> URL {
        // FNV-1a: stable across runs, no crypto dependency needed for a cache key.
        var hash: UInt64 = 0xcbf29ce484222325
        for byte in url.absoluteString.utf8 {
            hash ^= UInt64(byte)
            hash = hash &* 0x100000001b3
        }
        return directory.appendingPathComponent(String(format: "%016llx", hash))
    }

    /// Delivers a GdkTexture on the main queue, or nil if loading fails.
    static func load(_ url: URL, completion: @escaping (OpaquePointer?) -> Void) {
        let key = url.absoluteString
        if let texture = memory[key] { completion(texture); return }
        if failed.contains(key) { completion(nil); return }

        Task.detached(priority: .utility) {
            let data: Data?
            if url.isFileURL {
                data = try? Data(contentsOf: url)
            } else {
                let file = fileURL(for: url)
                if let onDisk = try? Data(contentsOf: file) {
                    data = onDisk
                } else if let (downloaded, _) = try? await URLSession.shared.data(from: url) {
                    try? downloaded.write(to: file)
                    data = downloaded
                } else {
                    data = nil
                }
            }
            DispatchQueue.main.async {
                // Texture creation must happen on the GTK thread.
                var texture: OpaquePointer?
                if let data = data, !data.isEmpty {
                    texture = data.withUnsafeBytes { buffer in
                        cp_texture_from_data(
                            buffer.baseAddress!.assumingMemoryBound(to: UInt8.self),
                            UInt(buffer.count))
                    }
                }
                if let texture = texture {
                    memory[key] = texture
                } else {
                    failed.insert(key)
                }
                completion(texture)
            }
        }
    }
}

/// A square cover image with an icon placeholder, like CachedAsyncImage.
/// The picture is a clipped overlay, so texture sizes never affect layout.
final class CoverView {
    private(set) var widget: Widget
    private var picture: Widget
    private var currentKey: String?

    init(size: Int32, icon: String = "audio-x-generic-symbolic") {
        var pictureOut: Widget = nil
        widget = cp_cover_new(size, icon, &pictureOut)
        picture = pictureOut

        // The widget tree is the only owner of cells built on the fly, so tie
        // this wrapper's lifetime to the widget or async loads hit a dead ref.
        let retained = Unmanaged.passRetained(self).toOpaque()
        let destroy: @convention(c) (UnsafeMutableRawPointer?) -> Void = { data in
            Unmanaged<CoverView>.fromOpaque(data!).release()
        }
        cp_widget_bind_object(widget, retained, unsafeBitCast(destroy, to: GDestroyNotify.self))
    }

    func setURL(_ url: URL?) {
        let key = url?.absoluteString
        guard key != currentKey else { return }
        currentKey = key
        cp_picture_set_texture(picture, nil)
        guard let url = url else { return }
        CoverView.loadInto(self, url: url, key: key)
    }

    private static func loadInto(_ view: CoverView, url: URL, key: String?) {
        CoverCache.load(url) { [weak view] texture in
            guard let view = view, view.currentKey == key, let texture = texture else { return }
            cp_picture_set_texture(view.picture, texture)
        }
    }
}
