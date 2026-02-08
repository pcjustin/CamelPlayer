import SwiftUI
import CoreAudio
import CamelPlayerCore

struct SettingsBarView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    var body: some View {
        HStack(spacing: 16) {
            // Device Picker
            HStack(spacing: 4) {
                Text("Device:")
                    .font(.caption)
                    .foregroundColor(.secondary)

                Picker("", selection: Binding(
                    get: { viewModel.currentDeviceID ?? 0 },
                    set: { viewModel.setOutputDevice($0) }
                )) {
                    ForEach(viewModel.audioDevices, id: \.id) { device in
                        Text(device.name).tag(device.id)
                    }
                }
                .frame(width: 200)
            }

            Divider()
                .frame(height: 20)

            // Playback Mode Picker
            HStack(spacing: 4) {
                Text("Mode:")
                    .font(.caption)
                    .foregroundColor(.secondary)

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
