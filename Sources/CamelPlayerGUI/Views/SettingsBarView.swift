import SwiftUI
import CoreAudio
import CamelPlayerCore

struct SettingsBarView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    private var localDevices: [OutputDevice] {
        viewModel.outputDevices.filter {
            if case .local = $0.type { return true }
            return false
        }
    }

    private var upnpDevices: [OutputDevice] {
        viewModel.outputDevices.filter {
            if case .upnp = $0.type { return true }
            return false
        }
    }

    var body: some View {
        HStack(spacing: 16) {
            // Device Picker
            HStack(spacing: 4) {
                Text("Device:")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .frame(width: 50, alignment: .trailing)

                Picker("", selection: Binding(
                    get: { viewModel.currentOutputDevice?.id ?? "" },
                    set: { deviceID in
                        if let device = viewModel.outputDevices.first(where: { $0.id == deviceID }) {
                            viewModel.setOutputDevice(device)
                        }
                    }
                )) {
                    Section(header: Text("Local Devices")) {
                        ForEach(localDevices) { device in
                            Label(device.name, systemImage: "speaker.wave.2")
                                .tag(device.id)
                        }
                    }

                    if !upnpDevices.isEmpty {
                        Section(header: Text("Network Devices (UPnP)")) {
                            ForEach(upnpDevices) { device in
                                Label(device.name, systemImage: "network")
                                    .tag(device.id)
                            }
                        }
                    }
                }
                .frame(width: 250)

                // Refresh UPnP devices button
                Button(action: {
                    viewModel.refreshUPnPDevices()
                }) {
                    Image(systemName: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
                .help("Refresh network devices")
            }

            Divider()
                .frame(height: 20)

            // Playback Mode Picker
            HStack(spacing: 4) {
                Text("Mode:")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .frame(width: 50, alignment: .trailing)

                Picker("", selection: Binding(
                    get: { viewModel.playbackMode },
                    set: { viewModel.setPlaybackMode($0) }
                )) {
                    Text("Sequential").tag(PlaybackMode.sequential)
                    Text("Loop All").tag(PlaybackMode.loop)
                    Text("Loop One").tag(PlaybackMode.loopOne)
                    Text("Shuffle").tag(PlaybackMode.shuffle)
                }
                .frame(width: 120)
            }

            Divider()
                .frame(height: 20)

            // Bit-Perfect Toggle
            Toggle(isOn: Binding(
                get: { viewModel.bitPerfectMode },
                set: { viewModel.setBitPerfectMode($0) }
            )) {
                Text("Bit-Perfect")
                    .font(.caption)
            }
            .toggleStyle(.checkbox)

            Spacer()

            // Add Files Button
            Button(action: {
                let urls = FilePickerHelper.selectAudioFiles()
                if !urls.isEmpty {
                    viewModel.addFiles(urls)
                }
            }) {
                Label("Add Files", systemImage: "plus")
            }
            .buttonStyle(.bordered)
            .help("Add audio files to playlist")

            // Add Folder Button
            Button(action: {
                let urls = FilePickerHelper.selectFolder()
                if !urls.isEmpty {
                    viewModel.addFiles(urls)
                }
            }) {
                Label("Add Folder", systemImage: "folder.badge.plus")
            }
            .buttonStyle(.bordered)
            .help("Add all audio files from a folder")
        }
    }
}
