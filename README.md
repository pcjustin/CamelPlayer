# CamelPlayer v2

CamelPlayer v2 is a new Windows-native music player built with C++20 and WinUI 3. It is the second-generation CamelPlayer project, distinct from the earlier CamelPlayer codebase.

The current implementation is an **MVP**: a stereo FLAC player with a bit-perfect output path, WASAPI exclusive, ASIO, and Kernel Streaming backends. It supports opening a file, playback, pause, stop, seeking, and live output/decoder changes. Four FLAC decoders remain available for comparison: FFmpeg, Symphonia, dr_flac, and libsndfile.

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

`decode` checks decoder output against STREAMINFO MD5. `capture` compares the engine's output with an independent libFLAC reference decode. Test audio covers 16/24/32-bit FLAC at rates up to 768 kHz, plus damaged, truncated, and missing-MD5 inputs. Decoder support varies, especially for 32-bit FLAC.

## Validation and limitations

Validation recorded **209 passed tests, 13 known-format skips, and zero failures** in each of Release and Debug. **40 hardware cases passed** through the engine and Server APIs, covering Chord/DAVE WASAPI, ASIO and KS, and Cirrus WASAPI and KS. Three Server concurrency/listener tests also passed 20 repetitions each (60 executions). Server capture tests compare 16/24-bit PCM from all four FLAC decoders with an independent libFLAC reference.

Hardware tests are disabled during ordinary runs because they play audio and require the named devices and a local `testdata/Sample_BeeMoved_96kHz24bit.flac` file, which is not distributed. Run them explicitly after building:

```powershell
.\build\Release\tests\tests.exe --gtest_also_run_disabled_tests --gtest_filter="*Hardware*"
```

The MVP is limited to stereo FLAC and unmodified integer output. Complete decodes starting at frame zero verify an available STREAMINFO MD5 and enforce known frame counts. After a nonzero seek, the remaining frame count is checked without comparing a partial digest to the whole-file MD5. The production decoder selection is still pending.

## Dependencies and repository contents

The WinUI control point submits versioned commands to a same-process C++ Server, which owns settings, device enumeration and reference-engine control. Snapshots and events carry command IDs and revisions to reject stale UI updates. The Rust engine migration and remote transport remain future work.

CamelPlayer-owned Server, engine and decoder adapters live in `src/Core`, the WinUI application in `src/App`, the CLI in `src/Tools/camelcli`, and the existing Rust Symphonia decoder FFI adapter in `rust/symphonia_ffi`. Tests and all required build configuration are included.

FFmpeg, dr_flac and libsndfile are restored through the pinned vcpkg manifest. Symphonia is restored through Cargo with a committed lockfile. The required ASIO SDK headers and their licenses are vendored in `third_party/asiosdk`.

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
