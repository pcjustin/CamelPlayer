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
    private var lastEmittedKey = ""
    private static let dictType = g_variant_type_new("a{sv}")

    init(model: PlayerModel) {
        self.model = model

        let busAcquired: GBusAcquiredCallback = { connection, _, data in
            Unmanaged<MPRIS>.fromOpaque(data!).takeUnretainedValue().busAcquired(connection)
        }
        _ = g_bus_own_name(G_BUS_TYPE_SESSION, Self.busName,
                           GBusNameOwnerFlags(rawValue: 0),
                           busAcquired, nil, nil,
                           Unmanaged.passUnretained(self).toOpaque(), nil)
    }

    private func busAcquired(_ connection: OpaquePointer?) {
        self.connection = connection
        var error: UnsafeMutablePointer<GError>?
        nodeInfo = g_dbus_node_info_new_for_xml(Self.introspectionXML, &error)
        guard let nodeInfo = nodeInfo else {
            if let error = error { g_error_free(error) }
            return
        }

        let methodCall: GDBusInterfaceMethodCallFunc = { _, _, _, _, method, _, invocation, data in
            let mpris = Unmanaged<MPRIS>.fromOpaque(data!).takeUnretainedValue()
            mpris.handleMethod(String(cString: method!))
            g_dbus_method_invocation_return_value(invocation, nil)
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
            _ = cp_dbus_register_object(connection, Self.objectPath, info,
                                        methodCall, getProperty, setProperty,
                                        Unmanaged.passUnretained(self).toOpaque())
        }
    }

    // GDBus delivers on the GLib main context, which is the GTK thread here.
    private func handleMethod(_ method: String) {
        switch method {
        case "PlayPause": model.togglePlayPause()
        case "Play": if !model.isPlaying { model.togglePlayPause() }
        case "Pause": model.pause()
        case "Stop": model.stop()
        case "Next": model.next()
        case "Previous": model.previous()
        default: break
        }
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
            return g_variant_new_int64(gint64(model.currentTime * 1_000_000))
        case "CanGoNext":
            return g_variant_new_boolean(model.canGoNext ? 1 : 0)
        case "CanGoPrevious":
            return g_variant_new_boolean(model.canGoPrevious ? 1 : 0)
        case "CanPlay", "CanPause", "CanControl":
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
        let track = model.currentPosition >= 0 ? model.currentPosition : 0
        add("mpris:trackid", g_variant_new_object_path("/org/camelplayer/track/\(track)"))
        if let duration = model.duration {
            add("mpris:length", g_variant_new_int64(gint64(duration * 1_000_000)))
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
            model.currentItem?.url.absoluteString ?? "",
            model.duration.map { String($0) } ?? "",
            String(model.canGoNext),
            String(model.canGoPrevious),
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
}
