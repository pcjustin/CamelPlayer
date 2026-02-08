import Foundation
import AppKit

struct FilePickerHelper {
    static func selectAudioFiles() -> [URL] {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowedContentTypes = [
            .mp3,
            .wav,
            .mpeg4Audio,
            .audio
        ]
        panel.title = "Select Audio Files"
        panel.message = "Choose one or more audio files to add to the playlist"

        guard panel.runModal() == .OK else {
            return []
        }

        return panel.urls
    }

    static func selectFolder() -> [URL] {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.title = "Select Folder"
        panel.message = "Choose a folder to scan for audio files"

        guard panel.runModal() == .OK,
              let folderURL = panel.url else {
            return []
        }

        return scanFolder(folderURL)
    }

    static func scanFolder(_ url: URL) -> [URL] {
        let fileManager = FileManager.default
        guard let enumerator = fileManager.enumerator(
            at: url,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles]
        ) else { return [] }

        let audioExtensions = ["mp3", "wav", "m4a", "flac", "alac", "aac", "aiff"]
        var audioFiles: [URL] = []

        for case let fileURL as URL in enumerator {
            if audioExtensions.contains(fileURL.pathExtension.lowercased()) {
                audioFiles.append(fileURL)
            }
        }

        return audioFiles.sorted { $0.path < $1.path }
    }
}
