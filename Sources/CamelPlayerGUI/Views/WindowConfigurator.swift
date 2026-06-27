import SwiftUI
import AppKit

/// Restores and autosaves the window frame (size + position) across launches.
struct WindowConfigurator: NSViewRepresentable {
    let autosaveName: String

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async {
            guard let window = view.window else { return }
            window.setFrameAutosaveName(autosaveName)
            window.setFrameUsingName(autosaveName)
        }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {}
}
