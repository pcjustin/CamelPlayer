import SwiftUI

struct NowPlayingView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    var body: some View {
        VStack(spacing: 16) {
            // Album Art
            Group {
                if let albumArt = viewModel.albumArt {
                    Image(nsImage: albumArt)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(width: 180, height: 180)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                        .shadow(radius: 4)
                } else {
                    Image(systemName: "music.note")
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: 120, height: 120)
                        .foregroundColor(.secondary)
                        .padding()
                        .background(
                            RoundedRectangle(cornerRadius: 12)
                                .fill(Color(NSColor.controlBackgroundColor))
                        )
                }
            }

            // Track Title
            Text(viewModel.currentItem?.title ?? "No Track Loaded")
                .font(.title2)
                .fontWeight(.semibold)
                .lineLimit(2)
                .multilineTextAlignment(.center)

            // Format Info
            HStack(spacing: 8) {
                if let formatInfo = viewModel.formatInfo {
                    Text(formatInfo)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }

                if viewModel.bitPerfectMode && viewModel.formatInfo != nil {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundColor(.green)
                        .font(.caption)
                        .help("Bit-perfect mode active")
                }
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical)
    }
}
