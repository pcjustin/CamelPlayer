# CamelPlayer

A native audio player featuring independent audio output device control and bit-perfect playback, for macOS (Core Audio / SwiftUI) and Linux (ALSA / GTK4).

## Features

### Core Audio Features

- **Bit-Perfect Playback**: Automatic hardware sample rate matching for zero-resampling playback
- **Independent Output Device Control**: Select and control audio output device independently from system settings using Core Audio
- **Multiple Audio Formats**: Support for MP3, WAV, M4A, FLAC, and ALAC (including high-res 192kHz/24bit)
- **Volume Control**: Independent volume control that doesn't affect system volume
- **Playback Modes**: Sequential, loop, loop-one, and shuffle

## Requirements

### macOS

- macOS 12.0 or later
- Swift 5.9 or later
- Xcode Command Line Tools

### Linux (Ubuntu)

- Swift 6 (`sudo apt install swiftlang` on Ubuntu 24.10 or later)
- GTK4, ALSA and libsndfile development packages:

```bash
sudo apt install libgtk-4-dev libasound2-dev libsndfile1-dev
```

## Installation

### Build from Source (macOS)

```bash
# Clone the repository
git clone https://github.com/yourusername/camelplayer.git
cd camelplayer

# Build the .app bundle (prompts to install to /Applications)
./build_gui_app.sh
```

### Build from Source (Linux)

```bash
git clone https://github.com/yourusername/camelplayer.git
cd camelplayer
swift build
```

## Usage

Launch the app on macOS:

```bash
open CamelPlayer.app
```

Launch the app on Linux:

```bash
swift run CamelPlayerGTK
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

- **SwiftUI**: Modern declarative UI framework for the GUI (macOS)
- **AVFoundation**: Audio file handling and playback engine (AVAudioEngine, AVAudioPlayerNode) (macOS)
- **Core Audio**: Independent output device control using AudioUnit API (macOS)
- **GTK4**: GUI toolkit for the Linux front end, called through a small C shim
- **ALSA + libsndfile**: Decoding and bit-perfect PCM output on Linux
- **Swift Package Manager**: Build system and dependency management

### Linux Port

`CamelPlayerCore` (playlist, UPnP discovery/browsing/playback, media server)
is shared between both platforms. Platform differences are confined to:

- `AudioPlayer`: AVAudioEngine on macOS, ALSA + libsndfile on Linux. The
  Linux backend opens the PCM device at the file's sample rate with software
  resampling disabled (S32_LE preferred), so `hw:` devices play bit-perfect.
- The GUI: SwiftUI (`CamelPlayerGUI`) on macOS, GTK4 (`CamelPlayerGTK`) on
  Linux. The GTK front end covers transport controls, seek, volume, the
  playlist and output device selection.

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
- **M4A**: MPEG-4 Audio (macOS only)
- **ALAC**: Apple Lossless Audio Codec (macOS only)
- **FLAC**: Free Lossless Audio Codec (macOS 10.13+)

macOS decodes through AVFoundation; Linux decodes through libsndfile, which
does not read M4A/ALAC.

## Known Limitations

### GUI
- No waveform visualization

### Linux
- No M4A/ALAC decoding
- No gapless playback for local files
- Selecting a `hw:` device opens it exclusively while playing

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
