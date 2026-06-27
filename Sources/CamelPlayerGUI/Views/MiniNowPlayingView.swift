import SwiftUI

/// Compact now-playing display (cover + title + album) for the bottom bar,
/// visible from every section.
struct MiniNowPlayingView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel

    var body: some View {
        HStack(spacing: 10) {
            cover
            VStack(alignment: .leading, spacing: 2) {
                Text(viewModel.currentItem?.title ?? "Not Playing")
                    .font(.callout).fontWeight(.medium).lineLimit(1)
                if let album = viewModel.currentAlbum, !album.isEmpty {
                    Text(album).font(.caption).foregroundColor(.secondary).lineLimit(1)
                }
            }
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder
    private var cover: some View {
        Group {
            if let url = viewModel.currentCoverURL {
                CachedAsyncImage(url: url) { placeholder }
            } else if let art = viewModel.albumArt {
                Image(nsImage: art).resizable().aspectRatio(contentMode: .fill)
            } else {
                placeholder
            }
        }
        .frame(width: 48, height: 48)
        .clipShape(RoundedRectangle(cornerRadius: 4))
    }

    private var placeholder: some View {
        RoundedRectangle(cornerRadius: 4)
            .fill(Color(NSColor.controlBackgroundColor))
            .overlay(Image(systemName: "music.note").foregroundColor(.secondary))
    }
}
