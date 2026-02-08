import SwiftUI

struct VolumeControlView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    var body: some View {
        HStack(spacing: 12) {
            // Speaker icon
            Image(systemName: volumeIcon)
                .foregroundColor(.secondary)
                .frame(width: 20)

            // Volume slider
            Slider(
                value: Binding(
                    get: { viewModel.volume },
                    set: { viewModel.setVolume($0) }
                ),
                in: 0.0...1.0
            )
            .frame(maxWidth: 300)

            // Volume percentage
            Text("\(Int(viewModel.volume * 100))%")
                .font(.caption)
                .foregroundColor(.secondary)
                .frame(width: 40, alignment: .trailing)
        }
        .frame(maxWidth: .infinity)
    }

    private var volumeIcon: String {
        if viewModel.volume == 0 {
            return "speaker.slash.fill"
        } else if viewModel.volume < 0.33 {
            return "speaker.wave.1.fill"
        } else if viewModel.volume < 0.66 {
            return "speaker.wave.2.fill"
        } else {
            return "speaker.wave.3.fill"
        }
    }
}
