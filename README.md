# CamelPlayer

A Swift audio player for macOS with both **CLI** and **native GUI** interfaces, featuring independent audio output device control and bit-perfect playback using Core Audio APIs.

## Features

### Core Audio Features

- **Bit-Perfect Playback**: Automatic hardware sample rate matching for zero-resampling playback
- **Independent Output Device Control**: Select and control audio output device independently from system settings using Core Audio
- **Multiple Audio Formats**: Support for MP3, WAV, M4A, FLAC, and ALAC (including high-res 192kHz/24bit)
- **Volume Control**: Independent volume control that doesn't affect system volume
- **Playback Modes**: Sequential, loop, loop-one, and shuffle

### Interface Options

- **Native macOS GUI**: SwiftUI-based graphical interface with reactive controls
- **Interactive CLI**: Terminal-based interface with command shortcuts
- **Command-Line Mode**: Direct playback via command-line arguments

## Requirements

- macOS 12.0 or later (GUI) / macOS 10.15 or later (CLI only)
- Swift 5.9 or later
- Xcode Command Line Tools

## Installation

### Build from Source

```bash
# Clone the repository
git clone https://github.com/yourusername/camelplayer.git
cd camelplayer

# Build CLI version
swift build -c release

# Install CLI to /usr/local/bin (optional)
sudo cp .build/release/CamelPlayer /usr/local/bin/camelplayer

# Build GUI version
./build_gui_app.sh
```

## Usage

### GUI Mode (Recommended)

Launch the native macOS app:

```bash
# Quick launch (auto-builds if needed)
./run_gui.sh

# Or open directly
open CamelPlayer.app

# Install to Applications folder
cp -r CamelPlayer.app /Applications/
```

#### GUI Features

- **Now Playing Display**: Shows current track, audio format, and bit-perfect status indicator
- **Playback Controls**: Large touch-friendly play/pause, next, previous, and stop buttons
- **Interactive Seek Bar**: Click or drag to seek to any position with time display
- **Volume Slider**: Visual volume control with percentage display
- **Playlist View**: Scrollable track list with current track highlighting
- **Settings Panel**:
  - Audio output device selection dropdown
  - Playback mode selector (Sequential, Loop All, Loop One, Shuffle)
  - Bit-perfect mode toggle
  - Add files and folders buttons
- **Error Handling**: User-friendly error alerts for unsupported formats or missing files

### Interactive CLI Mode

Start CamelPlayer in interactive mode for full terminal control:

```bash
camelplayer
```

or explicitly:

```bash
camelplayer interactive
```

#### Interactive Commands

**Playback Control:**
- `play` or `p` - Play current track
- `play <index>` - Play track at specified index
- `pause` - Pause playback
- `resume` or `r` - Resume playback
- `stop` or `s` - Stop playback
- `next` or `n` - Play next track
- `previous` or `prev` - Play previous track
- `seek <time>` - Seek to position (seconds or MM:SS format)

**Playlist Management:**
- `add <path>` or `a <path>` - Add file(s) or folder to playlist (automatically scans folders for audio files)
- `list` or `l` - Show playlist
- `remove <index>` or `rm <index>` - Remove item from playlist
- `clear` - Clear entire playlist

**Settings:**
- `volume <0-100>` or `vol <0-100>` - Set volume
- `mode <mode>` or `m <mode>` - Set playback mode
  - `sequential` or `seq` - Play tracks in order
  - `loop` or `l` - Loop entire playlist
  - `loopone` or `one` - Loop current track
  - `shuffle` or `sh` - Random playback
- `device [id]` or `dev [id]` - List or set output device
- `bitperfect [on|off]` or `bp [on|off]` - Enable/disable bit-perfect mode (default: ON)

**Information:**
- `info` or `i` - Show audio format and bit-perfect status
- `status` or `st` - Show playback status
- `help` or `h` or `?` - Show help message

**Other:**
- `quit` or `q` or `exit` - Exit interactive mode

### Command-Line Mode

#### Play a Single File

```bash
camelplayer play song.mp3
```

Wait for playback to finish:

```bash
camelplayer play --wait song.mp3
```

Play on specific output device:

```bash
camelplayer play --device 72 song.mp3
```

#### List Audio Devices

```bash
camelplayer devices
```

Output example:
```
Available audio output devices:

  [84] LG IPS FULLHD
* [72] MacBook Air Speakers
  [123] CADefaultDeviceAggregate-29235-0

Legend:
  * Current device
  → System default device
```

## Architecture

### Project Structure

```
CamelPlayer/
├── Sources/
│   ├── CamelPlayer/           # CLI executable entry point
│   │   └── main.swift
│   ├── CamelPlayerGUI/        # GUI executable (SwiftUI)
│   │   ├── CamelPlayerGUIApp.swift
│   │   ├── ViewModel/
│   │   │   └── PlaybackViewModel.swift
│   │   ├── Views/
│   │   │   ├── ContentView.swift
│   │   │   ├── NowPlayingView.swift
│   │   │   ├── PlaybackControlsView.swift
│   │   │   ├── SeekBarView.swift
│   │   │   ├── VolumeControlView.swift
│   │   │   ├── PlaylistView.swift
│   │   │   └── SettingsBarView.swift
│   │   └── Utilities/
│   │       ├── TimeFormatter.swift
│   │       └── FilePickerHelper.swift
│   ├── CamelPlayerCore/       # Core library (shared)
│   │   ├── AudioEngine/       # Audio playback engine
│   │   │   ├── AudioPlayer.swift
│   │   │   ├── OutputDeviceManager.swift
│   │   │   └── VolumeController.swift
│   │   ├── PlaybackControl/   # Playback orchestration
│   │   │   └── PlaybackController.swift
│   │   ├── Playlist/          # Playlist management
│   │   │   ├── Playlist.swift
│   │   │   └── PlaylistItem.swift
│   │   ├── Format/            # Audio format support
│   │   └── Display/           # Status display
│   └── CamelPlayerCLI/        # CLI interface layer
│       ├── Commands/          # ArgumentParser commands
│       │   ├── PlayCommand.swift
│       │   ├── DevicesCommand.swift
│       │   └── InteractiveCommand.swift
│       └── Interactive/       # Interactive mode
│           ├── InteractiveMode.swift
│           └── CommandParser.swift
├── Tests/
│   └── CamelPlayerCoreTests/
├── build_gui_app.sh           # GUI build script
└── run_gui.sh                 # GUI quick launcher
```

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
- **PlaybackController**: Core playback logic (shared with CLI)

### Core Technologies

- **SwiftUI**: Modern declarative UI framework for the GUI
- **AVFoundation**: Audio file handling and playback engine (AVAudioEngine, AVAudioPlayerNode)
- **Core Audio**: Independent output device control using AudioUnit API
- **Swift ArgumentParser**: Command-line interface and argument parsing
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
# Build CLI
swift build

# Build GUI
swift build --product CamelPlayerGUI

# Or use Xcode
open Package.swift
# Select CamelPlayerGUI or CamelPlayer scheme
```

### Run Tests

```bash
swift test
```

### Run in Debug Mode

```bash
# CLI
swift run CamelPlayer

# GUI (note: requires building .app bundle)
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
- No album art extraction (shows placeholder icon)
- No drag-and-drop support (use file picker buttons)
- No media key support (hardware play/pause keys)
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
