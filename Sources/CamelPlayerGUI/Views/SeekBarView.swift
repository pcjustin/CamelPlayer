import SwiftUI
import CamelPlayerCore

struct SeekBarView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel
    @State private var isSeekingManually = false
    @State private var seekPosition: Double = 0

    var body: some View {
        VStack(spacing: 8) {
            // Slider
            Slider(
                value: $seekPosition,
                in: 0...max(1, viewModel.duration ?? 1),
                onEditingChanged: { isEditing in
                    if isEditing {
                        isSeekingManually = true
                    } else {
                        isSeekingManually = false
                        viewModel.seek(to: seekPosition)
                    }
                }
            )
            .disabled((viewModel.duration ?? 0) <= 0 || viewModel.isStopped)
            .onChange(of: viewModel.currentTime) { newTime in
                if !isSeekingManually {
                    seekPosition = max(0, min(newTime, viewModel.duration ?? 0))
                }
            }

            // Time Labels
            HStack {
                Text(TimeFormatter.formatTime(viewModel.currentTime))
                    .font(.caption)
                    .foregroundColor(.secondary)

                Spacer()

                if let duration = viewModel.duration {
                    Text(TimeFormatter.formatTime(duration))
                        .font(.caption)
                        .foregroundColor(.secondary)
                } else {
                    Text("0:00")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
        }
    }
}
