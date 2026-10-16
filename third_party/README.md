# CamelPlayer v2 third-party dependencies

This directory holds upstream source distributed directly with CamelPlayer and supplementary license notices. CamelPlayer-owned adapters and bridges remain with the application source. The CamelPlayer brand assets are kept separately in `assets/`. Package-manager downloads, caches, and build outputs are not committed.

| Component | Source management | CamelPlayer integration |
|---|---|---|
| FFmpeg | `vcpkg.json` with a pinned baseline | `src/Core/audio/decode/FfmpegFlac.cpp` |
| dr_flac | vcpkg `drlibs` | `src/Core/audio/decode/DrFlac.cpp` |
| libsndfile | `vcpkg.json` with a pinned baseline | `src/Core/audio/decode/SndfileFlac.cpp` |
| Symphonia | `rust/symphonia_ffi/Cargo.toml` and `Cargo.lock` | Owned Rust C ABI bridge in `rust/symphonia_ffi`; C++ adapter in `src/Core/audio/decode/SymphoniaFlac.cpp` |
| libFLAC | vcpkg `libflac` | Test-audio encoding and independent reference decoding in `src/Core/audio/testing` |
| Steinberg ASIO SDK | Selected unmodified headers and licenses in `third_party/asiosdk` | `src/Core/audio/output/AsioOutput.cpp` |

The vcpkg installation is generated under `vcpkg_installed`; Cargo output is generated under `build/cargo`. Both are ignored by Git. The vendored ASIO SDK version, source URL, and archive checksum are recorded in [asiosdk/SOURCE.txt](asiosdk/SOURCE.txt).

Keep upstream license files with vendored code. Collected notices for native, Rust and Microsoft dependencies are in `licenses`; exact versions and source locations are in `dependencies.json`. Refresh snapshots with `msbuild/collect-licenses.ps1` after restoring dependencies. See [the distribution guide](../THIRD-PARTY-NOTICES.md) for source-access obligations and the unresolved licensing issue.

Changing the production decoder selection is a separate decision from organizing dependency sources. The MVP currently retains all four decoder adapters and their dependencies for comparison.
