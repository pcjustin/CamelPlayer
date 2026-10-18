# CamelPlayer v2

![CamelPlayer logo](assets/camelplayer-logo.jpg)

CamelPlayer v2 is a new Windows-native music player built with C++20 and WinUI 3. It is the second-generation CamelPlayer project, distinct from the earlier CamelPlayer codebase.

The current implementation is an **MVP**: a stereo FLAC, ALAC, WAV, AIFF, AAC-LC and MP3 player with a bit-perfect output path for lossless sources, WASAPI exclusive, ASIO, and Kernel Streaming backends. It supports opening a file, playback, pause, stop, seeking, and live output/decoder changes. Each format has its own decoder setting: FLAC can use FFmpeg, Symphonia, dr_flac or libsndfile; ALAC and AAC FFmpeg or Symphonia; WAV and AIFF FFmpeg, Symphonia, libsndfile or dr_wav; MP3 mpg123, FFmpeg, Symphonia or dr_mp3. AAC and MP3 decode to 24-bit PCM by default (16-bit per format), with declared encoder delay and padding trimmed. Mono files are duplicated to both channels bit for bit. With "convert when unsupported" (off by default) the player resamples with libsoxr, downmixes surround (ITU-R BS.775) or reduces depth with TPDF dither when a device does not take the file as it is; the indicator then shows the path as processed. Files are identified by content, not extension.

## Build

Requirements:

- Windows x64; the application targets Windows 10 version 1809 or later.
- Visual Studio 2026 with the Desktop development with C++ workload and Windows SDK 10.0.26100.0. The C++ WinUI app development tools component is not required.
- Rust stable with the `x86_64-pc-windows-msvc` target.
- Internet access for the initial vcpkg, NuGet, and Cargo dependency restore.
- At least 25 GB of available disk space is recommended for the initial build. Building FFmpeg can take about an hour; subsequent builds use caches.

Run from the repository root:

```powershell
.\build.cmd                          # Release build
.\build.cmd -Configuration Debug     # Debug build
.\build.cmd -Test                    # Build and run ordinary tests
.\build.cmd -Package                 # Create a local binary package
```

Executables are generated at `build/<Configuration>/App/CamelPlayer.exe`, `build/<Configuration>/camelcli/camelcli.exe`, and `build/<Configuration>/tests/tests.exe`. Configuration and logs are stored under `%LOCALAPPDATA%/CamelPlayer`. The executable and data-directory names remain `CamelPlayer` for compatibility; the project is named CamelPlayer v2.

## Command-line companion

```text
camelcli list-devices
camelcli play <file.flac> --backend ks --device 0 --decoder drflac
camelcli decode <file.flac>
camelcli capture <file.flac> out.raw
camelcli gen-test-audio [directory]
camelcli bench-decoders <directory> --runs 3
```

`decode` checks decoder output against STREAMINFO MD5. `capture` compares the engine's output with an independent libFLAC reference decode. Test audio covers 16/24-bit FLAC at rates up to 768 kHz, plus damaged, truncated, and missing-MD5 inputs. The test suite additionally generates 20/32-bit FLAC fixtures to check that unsupported depths are rejected cleanly.

## Validation and limitations

The current Release and Debug suites each ran **381 C++ tests: all passed, with no skips**. The Rust Engine suite passed all 35 tests. A historical run passed **40 hardware cases** through the engine and Server APIs, covering Chord/DAVE WASAPI, ASIO and KS, and Cirrus WASAPI and KS. Future hardware validation is limited to Chord/DAVE; do not rerun Cirrus WASAPI or KS. Three Server concurrency/listener tests also passed 20 repetitions each (60 executions). Server capture tests compare 16/24-bit PCM from all four FLAC decoders with an independent libFLAC reference.

Hardware tests are disabled during ordinary runs because they play audio and require the named devices and a local `testdata/Sample_BeeMoved_96kHz24bit.flac` file, which is not distributed. Run them explicitly after building:

```powershell
.\build\Release\tests\tests.exe --gtest_also_run_disabled_tests --gtest_filter="*Hardware*-*Cirrus*"
```

The MVP plays 16/24-bit lossless sources and AAC/MP3 to stereo output; without conversion it only repacks integers. FLAC, WAV and AIFF default to libsndfile, ALAC and AAC to Symphonia and MP3 to mpg123; an earlier saved FLAC choice is kept. DSD is recognized but not played yet. MP3 test files are encoded with LAME inside the test program only. Complete decodes starting at frame zero verify an available STREAMINFO MD5 and enforce known frame counts. After a nonzero seek, the remaining frame count is checked without comparing a partial digest to the whole-file MD5.

## Dependencies and repository contents

The WinUI control point submits versioned commands to a same-process C++ Server, which owns settings, device enumeration and reference-engine control. Snapshots and events carry command IDs and revisions to reject stale UI updates. A versioned Engine/Bridge C ABI and Rust Engine are integrated; the C++ OutputBridge wraps the existing output backends.

The WinUI application lives in `src/ControlPoint`, the Server in `src/Server`, the Rust Engine host and decoder adapters in `src/Engine`, the output backends in `src/OutputBridge`, shared types and utilities in `src/Common`, and the CLI in `src/Tools/camelcli`. The Rust workspace in `rust/` holds the Engine crate `rust/audio_engine` and the Symphonia decoder FFI adapter `rust/symphonia_ffi`. Tests are grouped into `tests/server_tests`, `tests/bridge_tests` and `tests/engine_tests`. The Rust Engine now owns the selected FLAC source session, enforces 16/24-bit input, track ranges and seek, and requests decoded frames through a versioned host callback ABI that reuses the four existing adapters. It reports STREAMINFO frame-count and MD5 verification states; full-source MD5 becomes verified only after complete decoding. Render output and the playback state machine are still under development. Tests and all required build configuration are included.

FFmpeg, dr_flac and libsndfile are restored through the pinned vcpkg manifest. Symphonia and the Rust Engine are restored through Cargo with one committed workspace lockfile and linked into one static library. The required ASIO SDK headers and their licenses are vendored in `third_party/asiosdk`.

The repository also includes third-party license/notice snapshots and a versioned [dependency inventory](third_party/dependencies.json). See [third-party notices](THIRD-PARTY-NOTICES.md) for the source and license information that must accompany a release.

Local planning documents, root-level development probes, private test music, downloaded package directories, build outputs and binary caches are excluded from Git. GitHub's automatic source archive contains the files committed at the release tag; it does not contain dependencies downloaded separately by package managers.

To refresh the checked-in notice snapshots after restoring dependencies:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File msbuild/collect-licenses.ps1
```

## License and release status

The project currently carries the [GNU GPL version 3](LICENSE). Third-party components retain their own licenses; the root license does not relicense Microsoft binaries or replace LGPL, MPL, MIT, BSD, or other notices.

**Public binary distribution is not yet cleared.** The combination of the GPLv3 ASIO SDK option with Microsoft-licensed Windows App SDK/WinUI binary packages still requires a licensing determination. A source archive or an exception applied only to CamelPlayer-owned code does not resolve third-party permissions.

For any future binary release, provide access to the exact corresponding source, dependency modifications and build information, and include all required notices. The local packaging command prepares files; it does not establish redistribution permission or publish a release.
