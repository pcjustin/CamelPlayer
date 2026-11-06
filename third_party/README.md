# CamelPlayer third-party dependencies

This directory holds upstream source distributed directly with CamelPlayer and supplementary license notices. CamelPlayer-owned adapters and bridges remain with the application source. The CamelPlayer brand assets are kept separately in `assets/`. Package-manager downloads, caches, and build outputs are not committed.

| Component | Source management | CamelPlayer integration |
|---|---|---|
| dr_flac | vcpkg `drlibs` | `src/Engine/decode/DrFlac.cpp` |
| dr_wav | vcpkg `drlibs` | `src/Engine/decode/DrWav.cpp` |
| mpg123 | `vcpkg.json` with a pinned baseline | `src/Engine/decode/Mpg123Decoder.cpp` |
| libsoxr | `vcpkg.json` with a pinned baseline | Resampling in the Rust Engine through `rust/audio_engine/src/resample.rs`; linked by the C++ executables |
| SQLite (FTS5) | `vcpkg.json` with a pinned baseline | `library.db` in the Server through `src/Server/Library/Sqlite.cpp` |
| LAME | `vcpkg.json` (`mp3lame`) with a pinned baseline | MP3 test fixtures in `tests/LameFixture.cpp`; linked into tests.exe only |
| libsndfile | `vcpkg.json` with a pinned baseline | `src/Engine/decode/SndfileDecoder.cpp` |
| DSD Nexus (libdsf, libdsdiff, libdst) | Overlay port `vcpkg-ports/dsd-nexus` pinned to upstream tag 0.1.82 by commit and SHA512; builds only these libraries and libsautil as the shared `dsdnexus.dll` | `src/Engine/decode/DsdReader.cpp` (not yet used for playback) |
| Symphonia | `rust/symphonia_ffi/Cargo.toml` and `rust/Cargo.lock` | Owned Rust C ABI bridge in `rust/symphonia_ffi`; C++ adapter in `src/Engine/decode/SymphoniaDecoder.cpp` |
| oxideav-dts | Pinned copy of its decoder in `rust/dts_core`, commit `ec55712` | C ABI in `rust/audio_engine/src/dts_abi.rs`; C++ stage in `src/Engine/decode/DtsStage.cpp` |
| libFLAC | vcpkg `libflac` | Test-audio encoding and independent reference decoding in `src/Engine/testing` |

The vcpkg installation is generated under `vcpkg_installed`; Cargo output is generated under `build/cargo`. Both are ignored by Git.

Keep upstream license files with vendored code. Collected notices for native, Rust and Microsoft dependencies are in `licenses`; exact versions and source locations are in `dependencies.json`. Refresh snapshots with `msbuild/collect-licenses.ps1` after restoring dependencies. See [the distribution guide](../THIRD-PARTY-NOTICES.md) for source-access obligations and the distribution status.

Changing the production decoder selection is a separate decision from organizing dependency sources. MP3 plays with mpg123 alone; the other formats keep their decoder adapters and dependencies for comparison.
