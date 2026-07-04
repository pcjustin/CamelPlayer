# CamelPlayer

A native macOS audio player featuring independent audio output device control and bit-perfect playback using Core Audio APIs.

## Features

### Core Audio Features

- **Bit-Perfect Playback**: Automatic hardware sample rate matching for zero-resampling playback
- **Independent Output Device Control**: Select and control audio output device independently from system settings using Core Audio
- **Multiple Audio Formats**: Support for MP3, WAV, M4A, FLAC, and ALAC (including high-res 192kHz/24bit)
- **Volume Control**: Independent volume control that doesn't affect system volume
- **Playback Modes**: Sequential, loop, loop-one, and shuffle

## Requirements

- macOS 12.0 or later
- Swift 5.9 or later
- Xcode Command Line Tools

## Installation

### Build from Source

```bash
# Clone the repository
git clone https://github.com/yourusername/camelplayer.git
cd camelplayer

# Build the .app bundle (prompts to install to /Applications)
./build_gui_app.sh
```

## Usage

Launch the app:

```bash
open CamelPlayer.app
```

### GUI Features

- **Now Playing Display**: Shows current track, audio format, and bit-perfect status indicator
- **Playback Controls**: Large touch-friendly play/pause, next, previous, and stop buttons
- **Interactive Seek Bar**: Click or drag to seek to any position with time display
- **Volume Slider**: Visual volume control with percentage display
- **Playlist View**: Scrollable track list with current track highlighting
- **Shuffle and Loop Toggles**: Independent shuffle and loop (off / all / one) controls in the transport bar
- **Settings Panel**:
  - Audio output device selection dropdown (local and UPnP renderers)
  - Add files and folders buttons
- **Error Handling**: User-friendly error alerts for unsupported formats or missing files

## Architecture

### GUI Architecture (MVVM Pattern)

The GUI uses a **Model-View-ViewModel** architecture with timer-based state synchronization:

```
SwiftUI Views ←→ PlaybackViewModel (ObservableObject) ←→ PlaybackController (Core API)
                      ↑
                   Timer (100ms polling)
```

**Key Components:**

- **Views**: SwiftUI views for the user interface (declarative, reactive)
- **PlaybackViewModel**: Bridges synchronous `PlaybackController` API with SwiftUI's reactive framework
  - Uses `@Published` properties to drive UI updates
  - Polls `PlaybackController` every 100ms to sync state
  - Handles user actions and errors
- **PlaybackController**: Core playback logic

### Core Technologies

- **SwiftUI**: Modern declarative UI framework for the GUI
- **AVFoundation**: Audio file handling and playback engine (AVAudioEngine, AVAudioPlayerNode)
- **Core Audio**: Independent output device control using AudioUnit API
- **Swift Package Manager**: Build system and dependency management

### Key Technical Implementation

#### Independent Output Device Control

The project uses Core Audio's AudioUnit API to control the output device independently from the system:

```swift
let audioUnit = engine.outputNode.audioUnit
var deviceID: AudioDeviceID = targetDeviceID
AudioUnitSetProperty(
    audioUnit,
    kAudioOutputUnitProperty_CurrentDevice,
    kAudioUnitScope_Global,
    0,
    &deviceID,
    UInt32(MemoryLayout<AudioDeviceID>.size)
)
```

This allows the player to route audio to a specific device regardless of the system's default output device setting.

#### Atomic loadAndPlay() for Race-Free Track Switching

To prevent UI flickering during track changes, `AudioPlayer` provides an atomic `loadAndPlay()` method:

```swift
public func loadAndPlay(url: URL) throws {
    // Set state to .playing immediately to avoid UI reading .stopped state
    state = .playing

    // Load and play atomically
    let file = try AVAudioFile(forReading: url)
    audioFile = file
    currentURL = url
    try playInternal()
}
```

This ensures that state remains `.playing` throughout the entire load-play cycle, eliminating race conditions with the ViewModel's polling timer.

## Development

### Build for Development

```bash
swift build

# Or use Xcode
open Package.swift
# Select the CamelPlayerGUI scheme
```

### Run Tests

```bash
swift test
```

### Run in Debug Mode

```bash
# The GUI must run from an .app bundle
./build_gui_app.sh
open CamelPlayer.app
```

## Supported Audio Formats

- **MP3**: MPEG Audio Layer 3
- **WAV**: Waveform Audio File Format
- **M4A**: MPEG-4 Audio
- **ALAC**: Apple Lossless Audio Codec
- **FLAC**: Free Lossless Audio Codec (macOS 10.13+)

All formats are supported natively through AVFoundation.

## Known Limitations

### GUI
- No waveform visualization

These features may be added in future versions.

## Troubleshooting

### GUI App Won't Launch

macOS GUI applications must be packaged as `.app` bundles. Do not run the binary directly:

```bash
# ❌ Wrong: will not show GUI window
./.build/debug/CamelPlayerGUI

# ✅ Correct: build .app bundle first
./build_gui_app.sh
open CamelPlayer.app
```

### Playback Issues

If tracks skip or won't play:
1. Check file format is supported (MP3, WAV, M4A, FLAC, ALAC)
2. Verify file is not corrupted
3. Check Console.app for error messages

## License

MIT License

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Acknowledgments

Built with Swift, Core Audio, and SwiftUI for macOS.
