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

    @State private var selectedServer: UPnPDevice?
    @State private var path: [Crumb] = []
    @State private var objects: [MediaObject] = []
    @State private var isLoading = false

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            content
        }
        .frame(minWidth: 420, minHeight: 480)
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 12) {
            if selectedServer != nil {
                Button(action: goBack) {
                    Image(systemName: "chevron.left")
                }
                .buttonStyle(.borderless)
            }

            Text(title)
                .font(.headline)
                .lineLimit(1)

            Spacer()

            if selectedServer != nil {
                Button {
                    guard let server = selectedServer, let current = path.last else { return }
                    Task { await viewModel.addContainerToPlaylist(server: server, objectID: current.objectID) }
                } label: {
                    Label("Add Folder", systemImage: "plus")
                }
                .buttonStyle(.bordered)
                .help("Add all tracks in this folder to the playlist")
            }

            Button("Done") { dismiss() }
        }
        .padding()
    }

    private var title: String {
        if let crumb = path.last { return crumb.title }
        return selectedServer?.friendlyName ?? "Network Servers"
    }

    // MARK: - Content

    @ViewBuilder
    private var content: some View {
        if selectedServer == nil {
            serverList
        } else if isLoading {
            VStack { Spacer(); ProgressView("Loading…"); Spacer() }
                .frame(maxWidth: .infinity)
        } else {
            objectList
        }
    }

    private var serverList: some View {
        Group {
            if viewModel.mediaServers.isEmpty {
                emptyState(icon: "server.rack", text: "No media servers found", hint: "Make sure your NAS / MinimServer is on the same network")
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
        Group {
            if objects.isEmpty {
                emptyState(icon: "folder", text: "Empty folder", hint: nil)
            } else {
                List(objects) { object in
                    row(for: object)
                        .contentShape(Rectangle())
                        .onTapGesture {
                            if object.isContainer { descend(into: object) }
                        }
                }
            }
        }
    }

    @ViewBuilder
    private func row(for object: MediaObject) -> some View {
        if object.isContainer {
            HStack {
                Image(systemName: "folder.fill").foregroundColor(.accentColor)
                Text(object.title).lineLimit(1)
                Spacer()
                if let n = object.childCount {
                    Text("\(n)").font(.caption).foregroundColor(.secondary)
                }
                Image(systemName: "chevron.right").font(.caption).foregroundColor(.secondary)
            }
        } else {
            HStack {
                Image(systemName: "music.note").foregroundColor(.secondary)
                VStack(alignment: .leading) {
                    Text(object.title).lineLimit(1)
                    if let artist = object.artist {
                        Text(artist).font(.caption).foregroundColor(.secondary).lineLimit(1)
                    }
                }
                Spacer()
                if let d = object.duration {
                    Text(TimeFormatter.formatTime(d)).font(.caption).foregroundColor(.secondary)
                }
            }
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
        .frame(maxWidth: .infinity)
        .padding()
    }

    // MARK: - Navigation

    private func open(_ server: UPnPDevice) {
        selectedServer = server
        path = [Crumb(objectID: "0", title: server.friendlyName)]
        reload()
    }

    private func descend(into container: MediaObject) {
        path.append(Crumb(objectID: container.id, title: container.title))
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
        }
    }

    private func reload() {
        guard let server = selectedServer, let current = path.last else { return }
        isLoading = true
        Task {
            let result = await viewModel.browse(server: server, objectID: current.objectID)
            objects = result
            isLoading = false
        }
    }
}
