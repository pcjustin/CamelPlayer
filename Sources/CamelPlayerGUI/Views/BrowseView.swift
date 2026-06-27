import SwiftUI
import CamelPlayerCore

private struct Crumb: Identifiable {
    let id = UUID()
    let objectID: String
    let title: String
}

struct BrowseView: View {
    @EnvironmentObject var viewModel: PlaybackViewModel
    @Environment(\.dismiss) private var dismiss

    private let pageSize = 200

    @State private var selectedServer: UPnPDevice?
    @State private var path: [Crumb] = []
    @State private var objects: [MediaObject] = []
    @State private var totalMatches = 0
    @State private var isLoading = false
    @State private var isLoadingMore = false
    @State private var sortCaps: [String] = []
    @State private var sortCriteria = ""

    var body: some View {
        VStack(spacing: 0) {
            header
            if selectedServer != nil {
                breadcrumb
                actionBar
            }
            Divider()
            content
        }
        .frame(minWidth: 460, minHeight: 520)
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 12) {
            if selectedServer != nil {
                Button(action: goBack) { Image(systemName: "chevron.left") }
                    .buttonStyle(.borderless)
            }
            Text(selectedServer == nil ? "Network Servers" : (selectedServer?.friendlyName ?? ""))
                .font(.headline).lineLimit(1)
            Spacer()
            Button("Done") { dismiss() }
        }
        .padding()
    }

    private var breadcrumb: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 4) {
                ForEach(Array(path.enumerated()), id: \.element.id) { index, crumb in
                    if index > 0 {
                        Image(systemName: "chevron.right").font(.caption2).foregroundColor(.secondary)
                    }
                    Button(crumb.title) { jump(to: index) }
                        .buttonStyle(.plain)
                        .foregroundColor(index == path.count - 1 ? .primary : .accentColor)
                        .lineLimit(1)
                }
            }
            .padding(.horizontal)
            .padding(.bottom, 6)
        }
    }

    private var actionBar: some View {
        HStack {
            if sortOptions.count > 1 {
                Menu {
                    ForEach(sortOptions, id: \.criteria) { option in
                        Button(option.label) {
                            sortCriteria = option.criteria
                            reload()
                        }
                    }
                } label: {
                    Label(currentSortLabel, systemImage: "arrow.up.arrow.down")
                }
                .frame(width: 160)
            }
            Spacer()
            Button {
                guard let server = selectedServer, let current = path.last else { return }
                Task { await viewModel.addContainerToPlaylist(server: server, objectID: current.objectID, sortCriteria: sortCriteria) }
            } label: {
                Label("Add Folder", systemImage: "plus")
            }
            .buttonStyle(.bordered)
            .help("Add all tracks in this folder to the playlist")
        }
        .padding(.horizontal)
        .padding(.bottom, 8)
    }

    // MARK: - Content

    @ViewBuilder
    private var content: some View {
        if selectedServer == nil {
            serverList
        } else if isLoading {
            VStack { Spacer(); ProgressView("Loading…"); Spacer() }.frame(maxWidth: .infinity)
        } else if objects.isEmpty {
            emptyState(icon: "folder", text: "Empty folder", hint: nil)
        } else {
            objectList
        }
    }

    private var serverList: some View {
        Group {
            if viewModel.mediaServers.isEmpty {
                emptyState(icon: "server.rack", text: "No media servers found",
                           hint: "Make sure your NAS / MinimServer is on the same network")
            } else {
                List(viewModel.mediaServers) { server in
                    HStack {
                        Image(systemName: "server.rack").foregroundColor(.accentColor)
                        VStack(alignment: .leading) {
                            Text(server.friendlyName)
                            Text(server.modelName).font(.caption).foregroundColor(.secondary)
                        }
                    }
                    .contentShape(Rectangle())
                    .onTapGesture { open(server) }
                }
            }
        }
    }

    private var objectList: some View {
        List {
            ForEach(objects) { object in
                row(for: object)
                    .contentShape(Rectangle())
                    .onTapGesture { if object.isContainer { descend(into: object) } }
                    .onAppear { loadMoreIfNeeded(object) }
            }
            if isLoadingMore {
                HStack { Spacer(); ProgressView(); Spacer() }
            }
        }
    }

    private func row(for object: MediaObject) -> some View {
        HStack(spacing: 10) {
            thumbnail(object)
            VStack(alignment: .leading, spacing: 2) {
                Text(object.title).lineLimit(1)
                if let artist = object.artist, !artist.isEmpty {
                    Text(artist).font(.caption).foregroundColor(.secondary).lineLimit(1)
                }
            }
            Spacer()
            if object.isContainer {
                if let n = object.childCount {
                    Text("\(n)").font(.caption).foregroundColor(.secondary)
                }
                Image(systemName: "chevron.right").font(.caption).foregroundColor(.secondary)
            } else if let d = object.duration {
                Text(TimeFormatter.formatTime(d)).font(.caption).foregroundColor(.secondary)
            }
        }
    }

    @ViewBuilder
    private func thumbnail(_ object: MediaObject) -> some View {
        let fallback = Image(systemName: object.isContainer ? "folder.fill" : "music.note")
        if let s = object.albumArtURI, let url = URL(string: s) {
            AsyncImage(url: url) { image in
                image.resizable().aspectRatio(contentMode: .fill)
            } placeholder: {
                fallback.foregroundColor(.secondary)
            }
            .frame(width: 36, height: 36)
            .clipShape(RoundedRectangle(cornerRadius: 4))
        } else {
            fallback
                .foregroundColor(object.isContainer ? .accentColor : .secondary)
                .frame(width: 36, height: 36)
        }
    }

    private func emptyState(icon: String, text: String, hint: String?) -> some View {
        VStack(spacing: 12) {
            Spacer()
            Image(systemName: icon).font(.system(size: 40)).foregroundColor(.secondary)
            Text(text).foregroundColor(.secondary)
            if let hint = hint {
                Text(hint).font(.caption).foregroundColor(.secondary).multilineTextAlignment(.center)
            }
            Spacer()
        }
        .frame(maxWidth: .infinity).padding()
    }

    // MARK: - Sort options

    private var sortOptions: [(label: String, criteria: String)] {
        var options: [(String, String)] = [("Default", "")]
        let known: [(cap: String, label: String, criteria: String)] = [
            ("dc:title", "Title", "+dc:title"),
            ("upnp:artist", "Artist", "+upnp:artist"),
            ("upnp:album", "Album", "+upnp:album"),
            ("upnp:originalTrackNumber", "Track #", "+upnp:originalTrackNumber")
        ]
        // Some servers (e.g. MinimServer) report empty SortCaps but still honor
        // standard criteria, so fall back to offering all when caps are empty.
        for entry in known where sortCaps.isEmpty || sortCaps.contains(entry.cap) {
            options.append((entry.label, entry.criteria))
        }
        return options
    }

    private var currentSortLabel: String {
        sortOptions.first { $0.criteria == sortCriteria }?.label ?? "Sort"
    }

    // MARK: - Navigation / loading

    private func open(_ server: UPnPDevice) {
        selectedServer = server
        path = [Crumb(objectID: "0", title: server.friendlyName)]
        sortCriteria = ""
        Task { sortCaps = await viewModel.sortCapabilities(server: server) }
        reload()
    }

    private func descend(into container: MediaObject) {
        path.append(Crumb(objectID: container.id, title: container.title))
        reload()
    }

    private func jump(to index: Int) {
        guard index < path.count - 1 else { return }
        path = Array(path.prefix(through: index))
        reload()
    }

    private func goBack() {
        if path.count > 1 {
            path.removeLast()
            reload()
        } else {
            selectedServer = nil
            path = []
            objects = []
            sortCaps = []
        }
    }

    private func reload() {
        guard let server = selectedServer, let current = path.last else { return }
        isLoading = true
        objects = []
        Task {
            let page = await viewModel.browse(
                server: server, objectID: current.objectID,
                startingIndex: 0, requestedCount: pageSize, sortCriteria: sortCriteria
            )
            objects = page?.objects ?? []
            totalMatches = page?.totalMatches ?? objects.count
            isLoading = false
        }
    }

    private func loadMoreIfNeeded(_ object: MediaObject) {
        guard object.id == objects.last?.id,
              !isLoadingMore,
              objects.count < totalMatches,
              let server = selectedServer, let current = path.last else { return }
        isLoadingMore = true
        Task {
            let page = await viewModel.browse(
                server: server, objectID: current.objectID,
                startingIndex: objects.count, requestedCount: pageSize, sortCriteria: sortCriteria
            )
            if let page = page {
                objects.append(contentsOf: page.objects)
                totalMatches = page.totalMatches
            }
            isLoadingMore = false
        }
    }
}
