import CGtk4
import CamelPlayerCore
import Foundation

/// MPRIS (org.mpris.MediaPlayer2) D-Bus service so desktop media keys and the
/// GNOME media panel control the player, the Linux counterpart of the macOS
/// MPRemoteCommandCenter / MPNowPlayingInfoCenter integration.
final class MPRIS {
    private static let busName = "org.mpris.MediaPlayer2.CamelPlayer"
    private static let objectPath = "/org/mpris/MediaPlayer2"

    private static let introspectionXML = """
    <node>
      <interface name='org.mpris.MediaPlayer2'>
        <method name='Raise'/>
        <method name='Quit'/>
        <property name='CanQuit' type='b' access='read'/>
        <property name='CanRaise' type='b' access='read'/>
        <property name='HasTrackList' type='b' access='read'/>
        <property name='Identity' type='s' access='read'/>
        <property name='SupportedUriSchemes' type='as' access='read'/>
        <property name='SupportedMimeTypes' type='as' access='read'/>
      </interface>
      <interface name='org.mpris.MediaPlayer2.Player'>
        <method name='Next'/>
        <method name='Previous'/>
        <method name='Pause'/>
        <method name='PlayPause'/>
        <method name='Stop'/>
        <method name='Play'/>
        <method name='Seek'><arg name='Offset' type='x' direction='in'/></method>
        <method name='SetPosition'>
          <arg name='TrackId' type='o' direction='in'/>
          <arg name='Position' type='x' direction='in'/>
        </method>
        <method name='OpenUri'><arg name='Uri' type='s' direction='in'/></method>
        <property name='PlaybackStatus' type='s' access='read'/>
        <property name='Rate' type='d' access='read'/>
        <property name='Shuffle' type='b' access='readwrite'/>
        <property name='Metadata' type='a{sv}' access='read'/>
        <property name='Volume' type='d' access='readwrite'/>
        <property name='Position' type='x' access='read'/>
        <property name='MinimumRate' type='d' access='read'/>
        <property name='MaximumRate' type='d' access='read'/>
        <property name='CanGoNext' type='b' access='read'/>
        <property name='CanGoPrevious' type='b' access='read'/>
        <property name='CanPlay' type='b' access='read'/>
        <property name='CanPause' type='b' access='read'/>
        <property name='CanSeek' type='b' access='read'/>
        <property name='CanControl' type='b' access='read'/>
      </interface>
    </node>
    """

    private let model: PlayerModel
    private var connection: OpaquePointer?
    private var nodeInfo: UnsafeMutablePointer<GDBusNodeInfo>?
    private var ownerID: UInt32 = 0
    private var registrations: [UInt32] = []
    private var lastEmittedKey = ""
    private static let dictType = g_variant_type_new("a{sv}")

    init(model: PlayerModel) {
        self.model = model

        let busAcquired: GBusAcquiredCallback = { connection, _, data in
            Unmanaged<MPRIS>.fromOpaque(data!).takeUnretainedValue().busAcquired(connection)
        }
        ownerID = g_bus_own_name(G_BUS_TYPE_SESSION, Self.busName,
                           GBusNameOwnerFlags(rawValue: 0),
                           busAcquired, nil, nil,
                           Unmanaged.passUnretained(self).toOpaque(), nil)
    }

    private func busAcquired(_ connection: OpaquePointer?) {
        unregisterObjects()
        if let connection = connection { _ = g_object_ref(UnsafeMutableRawPointer(connection)) }
        self.connection = connection
        lastEmittedKey = ""
        var error: UnsafeMutablePointer<GError>?
        nodeInfo = g_dbus_node_info_new_for_xml(Self.introspectionXML, &error)
        guard let nodeInfo = nodeInfo else {
            if let error = error { g_error_free(error) }
            return
        }

        let methodCall: GDBusInterfaceMethodCallFunc = { _, _, _, _, method, _, invocation, data in
            let mpris = Unmanaged<MPRIS>.fromOpaque(data!).takeUnretainedValue()
            if mpris.handleMethod(String(cString: method!)) {
                g_dbus_method_invocation_return_value(invocation, nil)
            } else {
                g_dbus_method_invocation_return_dbus_error(invocation,
                    "org.freedesktop.DBus.Error.NotSupported", "This operation is not supported")
            }
        }
        let getProperty: GDBusInterfaceGetPropertyFunc = { _, _, _, _, property, _, data in
            let mpris = Unmanaged<MPRIS>.fromOpaque(data!).takeUnretainedValue()
            return mpris.property(String(cString: property!))
        }
        let setProperty: GDBusInterfaceSetPropertyFunc = { _, _, _, _, property, value, _, data in
            let mpris = Unmanaged<MPRIS>.fromOpaque(data!).takeUnretainedValue()
            mpris.setProperty(String(cString: property!), value: value)
            return 1
        }

        for interface in ["org.mpris.MediaPlayer2", "org.mpris.MediaPlayer2.Player"] {
            let info = g_dbus_node_info_lookup_interface(nodeInfo, interface)
            let registration = cp_dbus_register_object(connection, Self.objectPath, info,
                                        methodCall, getProperty, setProperty,
                                        Unmanaged.passUnretained(self).toOpaque())
            if registration != 0 { registrations.append(registration) }
        }
    }

    // GDBus delivers on the GLib main context, which is the GTK thread here.
    private func handleMethod(_ method: String) -> Bool {
        switch method {
        case "PlayPause": model.togglePlayPause()
        case "Play": if !model.isPlaying { model.togglePlayPause() }
        case "Pause": model.pause()
        case "Stop": model.stop()
        case "Next": model.next()
        case "Previous": model.previous()
        default: return false
        }
        return true
    }

    private var playbackStatus: String {
        switch true {
        case model.isPlaying: return "Playing"
        case model.isPaused: return "Paused"
        default: return "Stopped"
        }
    }

    private func property(_ name: String) -> OpaquePointer? {
        switch name {
        case "CanQuit", "CanRaise", "HasTrackList", "CanSeek":
            return g_variant_new_boolean(0)
        case "Identity":
            return g_variant_new_string("CamelPlayer")
        case "SupportedUriSchemes", "SupportedMimeTypes":
            return g_variant_new_strv(nil, 0)
        case "PlaybackStatus":
            return g_variant_new_string(playbackStatus)
        case "Rate", "MinimumRate", "MaximumRate":
            return g_variant_new_double(1.0)
        case "Shuffle":
            return g_variant_new_boolean(model.shuffle ? 1 : 0)
        case "Metadata":
            return buildMetadata()
        case "Volume":
            return g_variant_new_double(Double(model.volume))
        case "Position":
            return g_variant_new_int64(microseconds(model.currentTime))
        case "CanGoNext":
            return g_variant_new_boolean(model.canGoNext ? 1 : 0)
        case "CanGoPrevious":
            return g_variant_new_boolean(model.canGoPrevious ? 1 : 0)
        case "CanPlay", "CanPause":
            return g_variant_new_boolean(!model.playlistItems.isEmpty && !model.currentTrackNeedsRenderer ? 1 : 0)
        case "CanControl":
            return g_variant_new_boolean(1)
        default:
            return nil
        }
    }

    private func setProperty(_ name: String, value: OpaquePointer?) {
        switch name {
        case "Volume":
            model.setVolume(Float(max(0, min(1, g_variant_get_double(value)))))
        case "Shuffle":
            model.setShuffle(g_variant_get_boolean(value) != 0)
        default:
            break
        }
    }

    private func buildMetadata() -> OpaquePointer? {
        let builder = g_variant_builder_new(Self.dictType)
        func add(_ key: String, _ value: OpaquePointer?) {
            g_variant_builder_add_value(builder,
                g_variant_new_dict_entry(g_variant_new_string(key), g_variant_new_variant(value)))
        }
        let trackID = model.currentItem.map {
            "/org/camelplayer/track/" + $0.id.uuidString.replacingOccurrences(of: "-", with: "_")
        } ?? "/org/mpris/MediaPlayer2/TrackList/NoTrack"
        add("mpris:trackid", g_variant_new_object_path(trackID))
        if let duration = model.duration {
            add("mpris:length", g_variant_new_int64(microseconds(duration)))
        }
        add("xesam:title", g_variant_new_string(model.currentItem?.title ?? ""))
        if let album = model.currentAlbum, !album.isEmpty {
            add("xesam:album", g_variant_new_string(album))
        }
        if let cover = model.currentCoverURL {
            add("mpris:artUrl", g_variant_new_string(cover.absoluteString))
        }
        let variant = g_variant_builder_end(builder)
        g_variant_builder_unref(builder)
        return variant
    }

    /// Called from the app tick; emits PropertiesChanged when visible state moves.
    func tick() {
        guard let connection = connection else { return }
        let key = [
            playbackStatus,
            model.currentItem?.id.uuidString ?? "",
            model.duration.map { String($0) } ?? "",
            String(model.canGoNext),
            String(model.canGoPrevious),
            String(model.volume),
            String(model.shuffle),
            String(model.currentTrackNeedsRenderer),
            model.currentCoverURL?.absoluteString ?? "",
        ].joined(separator: "|")
        guard key != lastEmittedKey else { return }
        lastEmittedKey = key

        let builder = g_variant_builder_new(Self.dictType)
        func add(_ key: String, _ value: OpaquePointer?) {
            g_variant_builder_add_value(builder,
                g_variant_new_dict_entry(g_variant_new_string(key), g_variant_new_variant(value)))
        }
        add("PlaybackStatus", g_variant_new_string(playbackStatus))
        add("Metadata", buildMetadata())
        add("CanGoNext", g_variant_new_boolean(model.canGoNext ? 1 : 0))
        add("CanGoPrevious", g_variant_new_boolean(model.canGoPrevious ? 1 : 0))
        add("Volume", g_variant_new_double(Double(model.volume)))
        add("Shuffle", g_variant_new_boolean(model.shuffle ? 1 : 0))
        let canPlay = !model.playlistItems.isEmpty && !model.currentTrackNeedsRenderer
        add("CanPlay", g_variant_new_boolean(canPlay ? 1 : 0))
        add("CanPause", g_variant_new_boolean(canPlay ? 1 : 0))
        let changed = g_variant_builder_end(builder)
        g_variant_builder_unref(builder)

        let children: [OpaquePointer?] = [
            g_variant_new_string("org.mpris.MediaPlayer2.Player"),
            changed,
            g_variant_new_strv(nil, 0),
        ]
        let arguments = children.withUnsafeBufferPointer {
            g_variant_new_tuple($0.baseAddress, gsize($0.count))
        }
        g_dbus_connection_emit_signal(
            connection, nil, Self.objectPath,
            "org.freedesktop.DBus.Properties", "PropertiesChanged",
            arguments, nil)
    }

    private func microseconds(_ seconds: TimeInterval) -> Int64 {
        guard seconds.isFinite, seconds >= 0 else { return 0 }
        return Int64(exactly: (seconds * 1_000_000).rounded(.down)) ?? 0
    }

    private func unregisterObjects() {
        if let connection = connection {
            for registration in registrations { g_dbus_connection_unregister_object(connection, registration) }
            g_object_unref(UnsafeMutableRawPointer(connection))
        }
        connection = nil
        registrations.removeAll()
        if let nodeInfo = nodeInfo { g_dbus_node_info_unref(nodeInfo) }
        nodeInfo = nil
    }

    deinit {
        unregisterObjects()
        if ownerID != 0 { g_bus_unown_name(ownerID) }
    }
}
