import Foundation

/// Whether Core diagnostic logging is enabled. Off by default so the library
/// does not spam stdout; set CAMEL_DEBUG in the environment to see UPnP/audio
/// discovery and playback traces.
private let camelDebugEnabled = ProcessInfo.processInfo.environment["CAMEL_DEBUG"] != nil

/// Lightweight gated logger for the Core library. Same call style as print().
func coreLog(_ message: @autoclosure () -> String) {
    if camelDebugEnabled {
        print(message())
    }
}
