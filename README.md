# CamelPlayer

A Swift CLI audio player for macOS featuring independent audio output device control using Core Audio APIs.

## Features

- **Bit-Perfect Playback**: Automatic hardware sample rate matching for zero-resampling playback
- **Independent Output Device Control**: Select and control audio output device independently from system settings using Core Audio
- **Multiple Audio Formats**: Support for MP3, WAV, M4A, FLAC, and ALAC (including high-res 192kHz/24bit)
- **Playlist Management**: Add files or entire folders, navigate through multiple tracks
- **Playback Control**: Play, pause, resume, stop, seek, next, and previous
- **Volume Control**: Independent volume control that doesn't affect system volume
- **Playback Modes**: Sequential, loop, loop-one, and shuffle
- **Hybrid CLI Interface**: Both command-line arguments and interactive mode
- **Real-time Status**: View playback state, audio format, and bit-perfect status

## Requirements

- macOS 10.15 or later
- Swift 5.9 or later
- Xcode Command Line Tools

## Installation

### Build from Source

```bash
# Clone the repository
git clone https://github.com/yourusername/camelplayer.git
cd camelplayer

# Build the project
swift build -c release

# Install to /usr/local/bin (optional)
sudo cp .build/release/CamelPlayer /usr/local/bin/camelplayer
```

## Usage

### Interactive Mode (Default)

Start CamelPlayer in interactive mode for full control:

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
* [72] MacBook Air的揚聲器
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
│   ├── CamelPlayer/           # Executable entry point
│   │   └── main.swift
│   ├── CamelPlayerCore/       # Core library
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
└── Tests/
    └── CamelPlayerCoreTests/
```

### Core Technologies

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

## Development

### Build for Development

```bash
swift build
```

### Run Tests

```bash
swift test
```

### Run in Debug Mode

```bash
swift run CamelPlayer
```

## Supported Audio Formats

- **MP3**: MPEG Audio Layer 3
- **WAV**: Waveform Audio File Format
- **M4A**: MPEG-4 Audio
- **ALAC**: Apple Lossless Audio Codec
- **FLAC**: Free Lossless Audio Codec (macOS 10.13+)

All formats are supported natively through AVFoundation.

## License

MIT License

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Acknowledgments

Built with Swift and Core Audio for macOS.
