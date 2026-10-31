# CamelPlayer third-party notices

CamelPlayer is licensed under the Mozilla Public License 2.0 ([LICENSE](LICENSE)); versions published before the change remain under GPLv3. Each dependency keeps its own license. This guide does not replace the complete upstream terms preserved under [third_party/licenses](third_party/licenses), and [third_party/dependencies.json](third_party/dependencies.json) records every dependency's exact version, license and source.

## CamelPlayer's own files

Every CamelPlayer file that can hold a comment starts with the MPL-2.0 header. Files that cannot (JSON, images, lockfiles) are covered by LICENSE. The logo files `assets/CamelPlayer.ico` and `assets/camelplayer-logo.jpg` are Justin Lu's own work, licensed under MPL-2.0 like the code. The recordings used for local testing are third-party material; they stay outside the repository and are never part of a release.

`msbuild/check-licenses.ps1`, run by `build.ps1` and the packaging script, fails when a file lacks the header, a dependency in vcpkg, Cargo.lock or packages.config is missing from the inventory, its license text or this guide, or a test recording is tracked.

## What a release contains

- `CamelPlayer-<version>-win-x64.zip`: the program, `LICENSE.txt` (MPL-2.0), this guide, `dependencies.json` and the complete `licenses` folder.
- `CamelPlayer-<version>-win-x64-third-party-source.zip`: the exact upstream source archives and vcpkg port trees (with their patches) of the LGPL and MPL libraries linked into the program, and the Symphonia crates. See [Source access and relinking](#source-access-and-relinking).
- CamelPlayer's own source is the repository at the release tag; GitHub's archive of the tag supplies it.

## Native libraries

Built through vcpkg (`vcpkg.json`, triplet `x64-windows-static-md`). "Linked" libraries are part of CamelPlayer.exe or ship beside it; test-only libraries are linked into the test program and never ship; build-only ones are used while building another library.

| Component | Package | Version | License | Use | Text |
|---|---|---|---|---|---|
| FFmpeg (avcodec, avformat, swresample) | `ffmpeg` | 9.0.2#1 | LGPL-2.1-or-later | linked | [Notice](third_party/licenses/native/ffmpeg.txt) |
| libsndfile | `libsndfile` | 1.2.2#2 | LGPL-2.1-or-later | linked | [Notice](third_party/licenses/native/libsndfile.txt) |
| dr_flac / dr_wav / dr_mp3 | `drlibs` | 2026-03-03 | Unlicense OR MIT-0 | linked | [Notice](third_party/licenses/native/drlibs.txt) |
| libFLAC | `libflac` | 1.5.0 | BSD-3-Clause | linked | [Notice](third_party/licenses/native/libflac.txt) |
| libogg | `libogg` | 1.3.6#1 | BSD-3-Clause | linked | [Notice](third_party/licenses/native/libogg.txt) |
| libvorbis | `libvorbis` | 1.3.7#4 | BSD-3-Clause | linked | [Notice](third_party/licenses/native/libvorbis.txt) |
| Opus | `opus` | 1.6.1 | BSD-3-Clause | linked | [Notice](third_party/licenses/native/opus.txt) |
| zlib | `zlib` | 1.3.2#2 | Zlib | linked | [Notice](third_party/licenses/native/zlib.txt) |
| WIL | `wil` | 1.0.260126.7 | MIT | linked | [Notice](third_party/licenses/native/wil.txt) |
| nlohmann-json | `nlohmann-json` | 3.12.0#2 | MIT | linked | [Notice](third_party/licenses/native/nlohmann-json.txt) |
| GoogleTest | `gtest` | 1.18.0 | BSD-3-Clause | tests only | [Notice](third_party/licenses/native/gtest.txt) |
| mpg123 | `mpg123` | 1.33.7#1 | LGPL-2.1-or-later | linked | [Notice](third_party/licenses/native/mpg123.txt) |
| LAME | `mp3lame` | 3.100#17 | LGPL-2.0-only | tests only | [Notice](third_party/licenses/native/mp3lame.txt) |
| libsoxr | `soxr` | 0.1.3#9 | LGPL-2.1-or-later | linked | [Notice](third_party/licenses/native/soxr.txt) |
| DSD Nexus (libdsf, libdsdiff, libdst), shared `dsdnexus.dll` | `dsd-nexus` | 0.1.82 | LGPL-2.1-or-later | linked | [Notice](third_party/licenses/native/dsd-nexus.txt) |
| SQLite (with FTS5) | `sqlite3` | 3.53.4#2 | Public domain (SQLite blessing) | linked | [Notice](third_party/licenses/native/sqlite3.txt) |
| TagLib | `taglib` | 2.3.1 | LGPL-2.1-only OR MPL-1.1 | linked | [Notice](third_party/licenses/native/taglib.txt) |
| UTF8-CPP | `utfcpp` | 4.2.1 | BSL-1.0 | linked | [Notice](third_party/licenses/native/utfcpp.txt) |
| libwebp | `libwebp` | 1.6.0#3 | BSD-3-Clause | linked | [Notice](third_party/licenses/native/libwebp.txt) |
| OpenCC | `opencc` | 1.4.1 | Apache-2.0 | linked | [Notice](third_party/licenses/native/opencc.txt) |
| marisa-trie | `marisa-trie` | 0.3.1 | BSD-2-Clause | linked | [Notice](third_party/licenses/native/marisa-trie.txt) |
| darts-clone | `darts-clone` | 1767ab87cffe#4 | BSD-2-Clause | linked | [Notice](third_party/licenses/native/darts-clone.txt) |
| RapidJSON | `rapidjson` | 2025-02-26#1 | MIT | linked | [Notice](third_party/licenses/native/rapidjson.txt) |
| TCLAP | `tclap` | 1.2.5 | MIT | build only | [Notice](third_party/licenses/native/tclap.txt) |

## Adapted code

The headphone crossfeed (`rust/audio_engine/src/crossfeed.rs`) follows the filters of libbs2b by Boris Mikhaylov, used under the MIT license ([notice](third_party/licenses/adapted/libbs2b.txt)). No libbs2b code is linked.

## Rust components

Locked in `rust/Cargo.lock` and linked into CamelPlayer.exe through the Rust Engine. Symphonia uses MPL-2.0 (full text in [MPL-2.0.txt](third_party/licenses/MPL-2.0.txt)). The other crates are used under their MIT option; `encoding_rs` also incorporates WHATWG data under BSD-3-Clause, whose COPYRIGHT and LICENSE-WHATWG are preserved with its MIT notice. Crate sources are at `https://static.crates.io/crates/<name>/<name>-<version>.crate` and match the checksums in Cargo.lock.

| Crate | Version | License | Text |
|---|---|---|---|
| `arrayvec` | 0.7.8 | MIT OR Apache-2.0 | [Notices](third_party/licenses/rust/arrayvec-0.7.8/) |
| `bitflags` | 1.3.2 | MIT/Apache-2.0 | [Notices](third_party/licenses/rust/bitflags-1.3.2/) |
| `bytemuck` | 1.25.2 | Zlib OR Apache-2.0 OR MIT | [Notices](third_party/licenses/rust/bytemuck-1.25.2/) |
| `cfg-if` | 1.0.5 | MIT OR Apache-2.0 | [Notices](third_party/licenses/rust/cfg-if-1.0.5/) |
| `core_detect` | 1.0.0 | MIT/Apache-2.0 | [Notices](third_party/licenses/rust/core_detect-1.0.0/) |
| `encoding_rs` | 0.8.42 | (Apache-2.0 OR MIT) AND BSD-3-Clause | [Notices](third_party/licenses/rust/encoding_rs-0.8.42/) |
| `extended` | 0.1.0 | MIT | [Notices](third_party/licenses/rust/extended-0.1.0/) |
| `lazy_static` | 1.5.1 | MIT OR Apache-2.0 | [Notices](third_party/licenses/rust/lazy_static-1.5.1/) |
| `log` | 0.4.34 | MIT OR Apache-2.0 | [Notices](third_party/licenses/rust/log-0.4.34/) |
| `multiversion_no_op` | 1.0.0 | Apache-2.0 OR MIT | [Notices](third_party/licenses/rust/multiversion_no_op-1.0.0/) |
| `rustversion` | 1.0.23 | MIT OR Apache-2.0 | [Notices](third_party/licenses/rust/rustversion-1.0.23/) |
| `scopeguard` | 1.2.0 | MIT OR Apache-2.0 | [Notices](third_party/licenses/rust/scopeguard-1.2.0/) |
| `simdutf8` | 0.1.5 | MIT OR Apache-2.0 | [Notices](third_party/licenses/rust/simdutf8-0.1.5/) |
| `symphonia` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-0.5.5/) |
| `symphonia-bundle-flac` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-bundle-flac-0.5.5/) |
| `symphonia-bundle-mp3` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-bundle-mp3-0.5.5/) |
| `symphonia-codec-aac` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-codec-aac-0.5.5/) |
| `symphonia-codec-alac` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-codec-alac-0.5.5/) |
| `symphonia-codec-pcm` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-codec-pcm-0.5.5/) |
| `symphonia-core` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-core-0.5.5/) |
| `symphonia-format-isomp4` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-format-isomp4-0.5.5/) |
| `symphonia-format-riff` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-format-riff-0.5.5/) |
| `symphonia-metadata` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-metadata-0.5.5/) |
| `symphonia-utils-xiph` | 0.5.5 | MPL-2.0 | [Notices](third_party/licenses/rust/symphonia-utils-xiph-0.5.5/) |
| `Rust standard library` | 1.99.1 | MIT OR Apache-2.0 | [Notices](third_party/licenses/rust-runtime/) |

## Microsoft components

NuGet packages from `src/ControlPoint/packages.config`, and the Visual C++ runtime DLLs the release ships beside the program. The Windows App SDK and WinUI binaries use Microsoft Software License Terms, independently of the MIT license on their public repository source. CppWinRT is MIT. The WebView2 SDK wrapper uses BSD-3-Clause with additional NOTICE material; this is not the separate browser runtime's license. The Windows SDK build tools carry no terms in their package, so the installed Windows SDK's license and third-party notices of the same release are kept. Microsoft-provided notices include Newtonsoft.Json, Antlr3.Runtime and StringTemplate4. The Visual C++ runtime folder also records the official source of its redistribution terms.

| Package | Version | Use | Text |
|---|---|---|---|
| `Microsoft.Web.WebView2` | 1.0.3179.45 | headers or runtime | [Notices](third_party/licenses/microsoft/Microsoft.Web.WebView2.1.0.3179.45/) |
| `Microsoft.Windows.CppWinRT` | 2.0.250303.1 | headers or runtime | [Notices](third_party/licenses/microsoft/Microsoft.Windows.CppWinRT.2.0.250303.1/) |
| `Microsoft.Windows.SDK.BuildTools` | 10.0.26100.4654 | build | [Notices](third_party/licenses/microsoft/Microsoft.Windows.SDK.BuildTools.10.0.26100.4654/) |
| `Microsoft.Windows.SDK.BuildTools.MSIX` | 1.7.20250829.1 | build | [Notices](third_party/licenses/microsoft/Microsoft.Windows.SDK.BuildTools.MSIX.1.7.20250829.1/) |
| `Microsoft.WindowsAppSDK.Base` | 1.8.251216001 | headers or runtime | [Notices](third_party/licenses/microsoft/Microsoft.WindowsAppSDK.Base.1.8.251216001/) |
| `Microsoft.WindowsAppSDK.DWrite` | 1.8.25122902 | headers or runtime | [Notices](third_party/licenses/microsoft/Microsoft.WindowsAppSDK.DWrite.1.8.25122902/) |
| `Microsoft.WindowsAppSDK.Foundation` | 1.8.260803002 | headers or runtime | [Notices](third_party/licenses/microsoft/Microsoft.WindowsAppSDK.Foundation.1.8.260803002/) |
| `Microsoft.WindowsAppSDK.InteractiveExperiences` | 1.8.260708001 | headers or runtime | [Notices](third_party/licenses/microsoft/Microsoft.WindowsAppSDK.InteractiveExperiences.1.8.260708001/) |
| `Microsoft.WindowsAppSDK.Runtime` | 1.8.260921001 | headers or runtime | [Notices](third_party/licenses/microsoft/Microsoft.WindowsAppSDK.Runtime.1.8.260921001/) |
| `Microsoft.WindowsAppSDK.WinUI` | 1.8.260803003 | headers or runtime | [Notices](third_party/licenses/microsoft/Microsoft.WindowsAppSDK.WinUI.1.8.260803003/) |
| `Microsoft.VCRedist (msvcp140, vcruntime140, vcruntime140_1)` | 14.51.36247.0 | runtime | [Notices](third_party/licenses/microsoft/MSVC-2026/) |

## Build tools

vcpkg host ports used while building the native libraries above. None is linked into CamelPlayer or included in a release. Visual Studio, CMake, NASM, Python, PowerShell and the MSYS2 tools that vcpkg downloads for its own use are part of the development environment and are not distributed either.

| vcpkg host port | Version | License | Text |
|---|---|---|---|
| `ffmpeg-bin2c` | 9.0.2 | MIT | [Notices](third_party/licenses/build-tools/ffmpeg-bin2c/) |
| `pkgconf` | 3.0.3#1 | ISC AND BSD-2-Clause AND BSD-4-Clause (getopt) AND GPL-2.0-or-later WITH Autoconf-exception-generic (pkg.m4) | [Notices](third_party/licenses/build-tools/pkgconf/) |
| `vcpkg-cmake` | 2025-08-07 | MIT | [Notices](third_party/licenses/build-tools/vcpkg-cmake/) |
| `vcpkg-cmake-config` | 2026-07-21 | MIT | [Notices](third_party/licenses/build-tools/vcpkg-cmake-config/) |
| `vcpkg-cmake-get-vars` | 2025-05-29 | MIT | [Notices](third_party/licenses/build-tools/vcpkg-cmake-get-vars/) |
| `vcpkg-msbuild` | 2023-08-08 | MIT | [Notices](third_party/licenses/build-tools/vcpkg-msbuild/) |
| `vcpkg-pkgconfig-get-modules` | 2024-04-03 | MIT | [Notices](third_party/licenses/build-tools/vcpkg-pkgconfig-get-modules/) |
| `vcpkg-tool-meson` | 1.9.0#10 | MIT (port scripts) AND Apache-2.0 (Meson) | [Notices](third_party/licenses/build-tools/vcpkg-tool-meson/) |
| `yasm` | 1.3.0#8 | BSD-2-Clause AND BSD-3-Clause AND (Artistic-1.0 OR GPL-2.0-only OR LGPL-2.0-only) (bitvect) | [Notices](third_party/licenses/build-tools/yasm/) |

## Online services

Artist pictures are looked up online only while the user allows it (asked on the first artist page; Settings › Interface › Artist pictures). CamelPlayer then sends an artist's or composer's name to [MusicBrainz](https://musicbrainz.org), the artist's Wikidata item to [Wikidata](https://www.wikidata.org) and the picture's file name to [Wikimedia Commons](https://commons.wikimedia.org), identifying itself as `CamelPlayer/<version> (https://github.com/pcjustin/CamelPlayer)` and asking MusicBrainz at most once a second. MusicBrainz and Wikidata publish their data under CC0. A Commons picture keeps its own free license (public domain, CC0, CC BY or CC BY-SA): the artist page names its author and license and links to the file's page, which holds the full terms. Pictures are kept in the user's library database and are never part of a release. No account or key is involved.

## Source access and relinking

The LGPL libraries (FFmpeg, libsndfile, mpg123, libsoxr, TagLib) are linked statically into CamelPlayer.exe; DSD Nexus is a separate DLL. Because CamelPlayer's complete source and build scripts are public, anyone can rebuild CamelPlayer against a modified version of any of them:

1. Get CamelPlayer's source at the release tag and `CamelPlayer-<version>-win-x64-third-party-source.zip` from the same release.
2. Install Visual Studio 2026 with the C++ desktop workload and the Rust toolchain recorded in `third_party/licenses/rust-runtime/toolchain.txt`.
3. Copy the library's `native/<name>/port` folder from the source zip into `third_party/vcpkg-ports/<name>`, which `vcpkg.json` already uses for overlay ports, and change its `portfile.cmake` to build your modified source (for example, point `SOURCE_PATH` at a local folder instead of the upstream download).
4. Run `.\build.ps1 -Configuration Release`. The new CamelPlayer.exe and package contain your version of the library.

For Symphonia (MPL-2.0), the source zip holds the exact crates; a modified crate can replace the locked one with a `[patch.crates-io]` entry in `rust/Cargo.toml`. The vcpkg port trees are also identified by the `port_source` tree hashes in the inventory, and the upstream archives by their SHA-512 checksums.

## Distribution status

CamelPlayer contains no Steinberg ASIO SDK code. Its ASIO host (`src/OutputBridge/AsioAbi.h`, `AsioOutput.cpp`) is an independent implementation of the driver interface, written from the interface's layout and values for interoperability. ASIO is a trademark of Steinberg Media Technologies GmbH; CamelPlayer is not affiliated with or endorsed by Steinberg.

MPL-2.0 permits combining CamelPlayer code with Microsoft-licensed WinUI/Windows App SDK binaries; those binaries stay under Microsoft's terms, which ship with the package. Binary releases provide the CamelPlayer source as MPL-2.0 requires (Source Code Form of covered files) and keep the LGPL and MPL obligations of the dependencies as described above.
