import SwiftUI

struct NowPlayingView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    var body: some View {
        VStack(spacing: 16) {
            // Album Art — remote cover for network tracks, local image otherwise
            Group {
                if let coverURL = viewModel.currentCoverURL {
                    CachedAsyncImage(url: coverURL) { placeholderArt }
                        .frame(width: 180, height: 180)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                        .shadow(radius: 4)
                } else if let albumArt = viewModel.albumArt {
                    Image(nsImage: albumArt)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(width: 180, height: 180)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                        .shadow(radius: 4)
                } else {
                    placeholderArt
                }
            }

            // Track Title
            Text(viewModel.currentItem?.title ?? "No Track Loaded")
                .font(.title2)
                .fontWeight(.semibold)
                .lineLimit(2)
                .multilineTextAlignment(.center)

            // Album Name
            if let album = viewModel.currentAlbum, !album.isEmpty {
                Text(album)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
                    .multilineTextAlignment(.center)
            }

            // Format Info
            HStack(spacing: 8) {
                if let formatInfo = viewModel.formatInfo {
                    Text(formatInfo)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }

                if let bitPerfect = viewModel.isBitPerfect {
                    Image(systemName: bitPerfect ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                        .foregroundColor(bitPerfect ? .green : .yellow)
                        .font(.caption)
                        .help(bitPerfect
                              ? "Bit-perfect: device sample rate matches the file"
                              : "Resampling: device sample rate does not match the file")
                }
            }

            // Favorite current track
            if let item = viewModel.currentItem {
                Button(action: { viewModel.toggleFavoriteTrack(item) }) {
                    let fav = viewModel.isFavoriteTrack(item.url.absoluteString)
                    Image(systemName: fav ? "star.fill" : "star")
                        .foregroundColor(fav ? .yellow : .secondary)
                }
                .buttonStyle(.plain)
                .help("Favorite track")
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical)
    }

    private var placeholderArt: some View {
        RoundedRectangle(cornerRadius: 12)
            .fill(Color(NSColor.controlBackgroundColor))
            .overlay(
                Image(systemName: "music.note")
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(width: 80, height: 80)
                    .foregroundColor(.secondary)
            )
            .frame(width: 180, height: 180)
    }
}
