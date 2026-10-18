# CamelPlayer v2 third-party dependencies

This directory holds upstream source distributed directly with CamelPlayer and supplementary license notices. CamelPlayer-owned adapters and bridges remain with the application source. The CamelPlayer brand assets are kept separately in `assets/`. Package-manager downloads, caches, and build outputs are not committed.

| Component | Source management | CamelPlayer integration |
|---|---|---|
| FFmpeg | `vcpkg.json` with a pinned baseline | `src/Engine/decode/FfmpegDecoder.cpp` |
| dr_flac | vcpkg `drlibs` | `src/Engine/decode/DrFlac.cpp` |
| dr_wav | vcpkg `drlibs` | `src/Engine/decode/DrWav.cpp` |
| dr_mp3 | vcpkg `drlibs` | `src/Engine/decode/DrMp3.cpp` |
| mpg123 | `vcpkg.json` with a pinned baseline | `src/Engine/decode/Mpg123Decoder.cpp` |
| LAME | `vcpkg.json` (`mp3lame`) with a pinned baseline | MP3 test fixtures in `tests/LameFixture.cpp`; linked into tests.exe only |
| libsndfile | `vcpkg.json` with a pinned baseline | `src/Engine/decode/SndfileDecoder.cpp` |
| Symphonia | `rust/symphonia_ffi/Cargo.toml` and `rust/Cargo.lock` | Owned Rust C ABI bridge in `rust/symphonia_ffi`; C++ adapter in `src/Engine/decode/SymphoniaDecoder.cpp` |
| libFLAC | vcpkg `libflac` | Test-audio encoding and independent reference decoding in `src/Engine/testing` |
| Steinberg ASIO SDK | Selected unmodified headers and licenses in `third_party/asiosdk` | `src/OutputBridge/AsioOutput.cpp` |

The vcpkg installation is generated under `vcpkg_installed`; Cargo output is generated under `build/cargo`. Both are ignored by Git. The vendored ASIO SDK version, source URL, and archive checksum are recorded in [asiosdk/SOURCE.txt](asiosdk/SOURCE.txt).

Keep upstream license files with vendored code. Collected notices for native, Rust and Microsoft dependencies are in `licenses`; exact versions and source locations are in `dependencies.json`. Refresh snapshots with `msbuild/collect-licenses.ps1` after restoring dependencies. See [the distribution guide](../THIRD-PARTY-NOTICES.md) for source-access obligations and the unresolved licensing issue.

Changing the production decoder selection is a separate decision from organizing dependency sources. The MVP currently retains all four decoder adapters and their dependencies for comparison.
