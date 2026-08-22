import Foundation

/// Extensions accepted by the file pickers, drag-and-drop and folder scanning
/// on both front ends. The GTK dialog filter in Sources/CGtk4/shim.h duplicates
/// this list in C; AudioFileTypesTests asserts the two stay in sync.
public let audioFileExtensions = ["mp3", "wav", "m4a", "flac", "alac", "aac", "aiff"]
