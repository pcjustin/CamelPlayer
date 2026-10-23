# CamelPlayer v2 third-party notices

The project license, MPL-2.0 ([LICENSE](LICENSE)), applies to CamelPlayer-covered code; versions published before the change remain under GPLv3. Each dependency retains its own applicable license. This guide does not replace the complete upstream terms preserved under [third_party/licenses](third_party/licenses).

## Repository contents

Keep the application, core, CLI and Rust bridge source, solution/project files, manifests, lockfile, build/package scripts, and tests referenced by the solution in the repository.

Also keep the collected copyright/license notices, runtime notices, Microsoft package terms, and [dependency inventory](third_party/dependencies.json). The inventory lists exact versions and source locations; native archive checksums and vcpkg port-tree locations identify the sources and patches used. Rust archive checksums are recorded in `rust/Cargo.lock`.

Downloaded static libraries, NuGet installations, complete SDKs, private music, root-level development probes and build caches are not committed. Their absence does not remove obligations when distributing a program incorporating third-party code.

## Native libraries

| Component | Applicable license | Complete notice |
|---|---|---|
| FFmpeg | Installed build: LGPL 2.1; upstream LGPL-2.1-or-later | [Notice](third_party/licenses/native/ffmpeg.txt) |
| libsndfile | LGPL-2.1-or-later | [Notice](third_party/licenses/native/libsndfile.txt) |
| mpg123 | LGPL-2.1-or-later | [Notice](third_party/licenses/native/mpg123.txt) |
| libsoxr | LGPL-2.1-or-later | [Notice](third_party/licenses/native/soxr.txt) |
| SQLite 3.53.4 (with FTS5) | Public domain (SQLite blessing) | [Notice](third_party/licenses/native/sqlite3.txt) |
| TagLib | LGPL-2.1-only OR MPL-1.1 | [Notice](third_party/licenses/native/taglib.txt) |
| UTF8-CPP | BSL-1.0 | [Notice](third_party/licenses/native/utfcpp.txt) |
| DSD Nexus (libdsf, libdsdiff, libdst) | LGPL-2.1-or-later; built as the shared library `dsdnexus.dll` | [Notice](third_party/licenses/native/dsd-nexus.txt) |
| dr_flac / dr_wav / dr_mp3 (drlibs) | Unlicense OR MIT-0 | [Notice](third_party/licenses/native/drlibs.txt) |
| libFLAC | BSD-3-Clause library code | [Notice](third_party/licenses/native/libflac.txt) |
| libogg | BSD-3-Clause | [Notice](third_party/licenses/native/libogg.txt) |
| libvorbis | BSD-3-Clause | [Notice](third_party/licenses/native/libvorbis.txt) |
| Opus | BSD-3-Clause and accompanying patent statements | [Notice](third_party/licenses/native/opus.txt) |
| zlib | Zlib | [Notice](third_party/licenses/native/zlib.txt) |
| nlohmann-json | MIT | [Notice](third_party/licenses/native/nlohmann-json.txt) |
| WIL | MIT | [Notice](third_party/licenses/native/wil.txt) |
| GoogleTest | BSD-3-Clause, tests only | [Notice](third_party/licenses/native/gtest.txt) |
| LAME | LGPL-2.0-only, tests only (encodes MP3 fixtures; not linked into the player) | [Notice](third_party/licenses/native/mp3lame.txt) |

## Rust components

Symphonia and its FLAC, core, metadata and Xiph utility crates use MPL-2.0. The other locked crates offer an MIT license option; `encoding_rs` additionally incorporates WHATWG data under BSD-3-Clause. Preserve its COPYRIGHT and LICENSE-WHATWG as well as its MIT notice.

Per-crate license/copyright files are in [licenses/rust](third_party/licenses/rust), the inventory lists all 17 third-party crates, and the MPL text is in [MPL-2.0.txt](third_party/licenses/MPL-2.0.txt). The installed Rust standard library has additional notices in [COPYRIGHT-library.html](third_party/licenses/rust-runtime/COPYRIGHT-library.html), with its toolchain version recorded alongside it. This upstream inventory can include components absent from a particular linked executable.

## Microsoft components

Windows App SDK and WinUI NuGet binaries use Microsoft Software License Terms, independently of the MIT license on public repository source. CppWinRT is MIT. The WebView2 SDK wrapper in this dependency version uses BSD-3-Clause with additional NOTICE material; this is not the separate browser runtime's license. Windows SDK build tools and the Visual C++ runtime have separate Microsoft terms.

Exact package license/NOTICE snapshots are under [licenses/microsoft](third_party/licenses/microsoft), including the Visual C++ V14 2026 redistribution terms and their official source record. Microsoft-provided notices include Newtonsoft.Json, Antlr3.Runtime and StringTemplate4. Preserve the complete notices for distributed components.

## Source access for binary releases

GitHub's automatic archive can supply the project source at a matching release tag. It does not automatically include vcpkg/Cargo dependency sources or patches. Supply the exact applicable upstream sources and modifications, or compliant equivalent source access with clear directions beside the binary download. A generic homepage or version list alone is not a complete source-access procedure.

The inventory's native `port_source` fields identify exact vcpkg port trees, including patches. Native `sources` checksums identify upstream archives. Rust source archive locations are version-specific and their checksums can be verified against Cargo.lock. Package-manager installation directories do not need to be committed merely to retain these records.

Release binaries need copies of the required notices. Include this guide, the root MPL-2.0 text and collected notice directories. Where LGPL static linking requires a rebuilding/relinking route, provide the necessary source/object materials and scripts under appropriate terms. MPL-covered source must also be available to recipients.

## Distribution status

CamelPlayer contains no Steinberg ASIO SDK code. Its ASIO host (`src/OutputBridge/AsioAbi.h`, `AsioOutput.cpp`) is an independent implementation of the driver interface, written from the interface's layout and values for interoperability. ASIO is a trademark of Steinberg Media Technologies GmbH; CamelPlayer is not affiliated with or endorsed by Steinberg.

MPL-2.0 permits combining CamelPlayer code with Microsoft-licensed WinUI/Windows App SDK binaries; those binaries stay under Microsoft's terms, which ship with the package. Binary releases provide the CamelPlayer source as MPL-2.0 requires (Source Code Form of covered files) and keep the LGPL/MPL obligations of the dependencies.
