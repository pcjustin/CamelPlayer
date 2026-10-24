# CamelPlayer v2

![CamelPlayer logo](assets/camelplayer-logo.jpg)

CamelPlayer v2 is a new Windows-native music player built with C++20 and WinUI 3. It is the second-generation CamelPlayer project, distinct from the earlier CamelPlayer codebase.

The current implementation is an **MVP**: a stereo FLAC, ALAC, WAV, AIFF, AAC-LC, MP3 and DSD (DSF/DSDIFF) player with a bit-perfect output path for lossless sources, WASAPI exclusive and shared, ASIO, and Kernel Streaming backends. WASAPI shared mode always converts to the Windows mixer format and is shown as processed; WASAPI can follow the Windows default device, moving an open stream to a new default and resuming only active playback, while an explicitly selected device pauses when it disappears. Each backend has its own fixed sample rate (choosing one permits resampling PCM to it), predecode amount, silence after a format change and release of the device after a pause; WASAPI also sets the polling buffer, and WASAPI and KS the MMCSS task and priority. It supports opening a file, playback, pause, stop, seeking, and live output/decoder changes. Each format has its own decoder setting: FLAC can use FFmpeg, Symphonia, dr_flac or libsndfile; ALAC and AAC FFmpeg or Symphonia; WAV and AIFF FFmpeg, Symphonia, libsndfile or dr_wav; MP3 mpg123, FFmpeg, Symphonia or dr_mp3. AAC and MP3 decode to 24-bit PCM by default (16-bit per format), with declared encoder delay and padding trimmed. Mono files are duplicated to both channels bit for bit. With "convert when unsupported" (off by default) the player resamples with libsoxr, downmixes surround (ITU-R BS.775) or reduces depth with TPDF dither when a device does not take the file as it is; the indicator then shows the path as processed. Files are identified by content, not extension.

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
camelcli bench-decoders <directory> --runs 3   # every recognized format; pure decode, separate FLAC MD5, seek checks
```

`decode` checks decoder output against STREAMINFO MD5. `capture` compares the engine's output with an independent libFLAC reference decode. Test audio covers 16/24-bit FLAC at rates up to 768 kHz, plus damaged, truncated, and missing-MD5 inputs. The test suite additionally generates 20/32-bit FLAC fixtures to check that unsupported depths are rejected cleanly.

## Validation and limitations

The latest full Release run passed **568/570 enabled C++ tests**; 109 hardware and opt-in NAS tests remain disabled. The scan-batch timeout passed when rerun separately; the existing 100k-track search p95 remained slightly over its 100 ms target at 102.5 ms (154.9 ms during concurrent compilation). The final M4-R03 artwork regressions passed **14/14**, including CD-subfolder covers and misleading source extensions; the user confirmed all covers display correctly. The earlier Debug full run passed 562/563: the 100k-track search latency target (p95 < 100 ms) is defined for optimized builds and measures 332 ms unoptimized. M3-R06 final affected-module regressions passed **137/137 in Debug**, and three opt-in NAS checks passed in both configurations, including ten physical local/NAS relocation scenarios and actual Server playback-state gating. The Rust Engine suite passed all 45 tests.

M3-R07 performance (Release, caches not flushed): warm 100k-track grouped-search first page p95 79.5–85.4 ms; synthetic 100k-track local-SSD first scan 364 s (target ≤ 15 minutes). A read-only full scan of an 86,684-file NAS share (75,507 tracks) took 27.1 minutes, and unchanged rescans took 19–20 seconds; NAS scan time is recorded rather than targeted. The latest hardware run covered **91 hardware cases** through the engine and Server APIs: FLAC playback controls on Chord/DAVE WASAPI, ASIO and KS and Cirrus WASAPI and KS; WAV, ALAC, AAC, MP3, WASAPI shared mode and DSD (native, DoP, DSD to PCM) wherever the device can play them; and DSD64 to DSD512 on DAVE, where DSD512 DoP (a 1411.2 kHz carrier) is refused or reported as a driver failure rather than assumed. KS has no native DSD: Windows defines no KS DSD format, so KS plays DSD as DoP or permitted DSD to PCM. Hardware validation covers Chord/DAVE (WASAPI, ASIO, KS) and the built-in Cirrus codec (WASAPI, KS): Cirrus takes 24-in-32 up to 192 kHz, a container DAVE refuses. Cirrus has no DSD mode, so it only receives DSD converted to PCM, never DoP or native DSD. A separate DAVE run of the new output parameters (polling buffer, MMCSS, fixed rate, silence after a DoP-to-PCM switch, release after pause) passed 5/5. Three Server concurrency/listener tests also passed 20 repetitions each (60 executions). Server capture tests compare 16/24-bit PCM from all four FLAC decoders with an independent libFLAC reference.

Hardware tests are disabled during ordinary runs because they play audio and require the named devices and local `testdata/` files (FLAC, WAV, ALAC, AAC, MP3 and DSF samples), which are not distributed. Lower the volume, then run them explicitly after building:

```powershell
.\build\Release\tests\tests.exe --gtest_also_run_disabled_tests --gtest_filter="*Hardware*"
```

NAS validation is also opt-in: set `CAMEL_NAS_ROOT` to an authorized share or album and select `LibraryNas.DISABLED_ReadOnlyFullAndIncrementalScan`. The `LibraryNas.DISABLED_IsolatedChangesOverflowAndReconnection` and `LibraryNas.DISABLED_UniqueFingerprintsPreserveMoveHistory` cases create and clean their own temporary folders on that share; run them only with permission to create test files. All index databases and reports stay under local `build/testtmp/nas-library`.

A real Release SMB run passed representative FLAC/DSD full and unchanged incremental scans, 2,000 change events with a zero-byte notification-overflow signal, recursive rename/delete detection and temporary-directory unavailability/reconnection without losing records. Tag edits did not trigger an index update within 120 seconds in this environment; a test-only 2-second polling interval recovered them. The production polling default remains 30 minutes, so external changes missed by notifications can remain stale until the next poll. M3-R07 measured the 100k-track Release Server search first-page p95 at 85.4 ms; one/two-character fallback p95 was 1,146/939 ms. A synthetic local-SSD first scan completed in 897.3 s, with its first 500-file batch at 633.3 s. The authorized NAS full scan took 10,507 s (86,684 files seen, 147 diagnostics); an unchanged rescan took 192.7 s (0 updates, 4 diagnostics). At the user's request, the full NAS fingerprint run was stopped after 157 of 86,680 files (3.21 GB) in 438 s; the observed rate projects about 109 hours for 2.87 TB. The local index passed SQLite integrity check after stopping. See the partial R07 report under `build/testtmp/nas-library`; physical network interruption remains untested. The NAS follow-up changes passed 30 related tests in both Debug and Release.

The MVP plays 16/24-bit lossless sources and AAC/MP3 to stereo output; without conversion it only repacks integers. FLAC, WAV and AIFF default to libsndfile, ALAC and AAC to Symphonia and MP3 to mpg123; an earlier saved FLAC choice is kept. DSF and DSDIFF files are read as raw DSD through DSD Nexus and play bit for bit as native DSD on ASIO (the default there, `asio.dsdOutput`) or as DoP on every backend; DoP needs a carrier of DSD rate / 16, so DSD512 DoP plays only where the device reports 1411.2 kHz. DSD becomes PCM only with explicit permission: a backend's DSD output set to "to PCM", or "convert DSD when unsupported" (off by default) after native/DoP failed. It is then decimated to 352.8 kHz by a multistage FIR filter (100% DSD modulation = 0 dBFS), resampled with libsoxr to the configured 88.2/176.4/352.8 kHz and quantized once; multichannel DSD also needs "convert when unsupported" to downmix. DST decompression stays disabled. MP3 test files are encoded with LAME inside the test program only. Complete decodes starting at frame zero verify an available STREAMINFO MD5 and enforce known frame counts. After a nonzero seek, the remaining frame count is checked without comparing a partial digest to the whole-file MD5.

## Dependencies and repository contents

The WinUI control point submits versioned commands to a same-process C++ Server, which owns settings, device enumeration and reference-engine control. Snapshots and events carry command IDs and revisions to reject stale UI updates. A versioned Engine/Bridge C ABI and Rust Engine are integrated; the C++ OutputBridge wraps the existing output backends.

The internal Server library service normalizes overlapping roots and aliases, skips reparse points to prevent cycles and escapes, and scans supported audio plus CUE, image and LRC sidecars into local SQLite storage. TagLib reads file tags, multivalued artists/composers/genres and embedded-cover presence; the existing decoder registry validates playback format scope and supplies stream information. FLAC STREAMINFO and Vorbis comments are read without opening a decoder or loading embedded artwork. MP3 files that declare no length are indexed with an estimate, which the first mpg123 playback replaces with the exact length. Scans publish batches of 500 files and reread metadata only when a file changes. Valid external CUE sheets take precedence over embedded FLAC CUESHEET blocks and expand into stable virtual tracks with INDEX 01 sample ranges; the library view hides the duplicate whole-file track and hidden lead-in. Stored inter-track pregap belongs to the preceding track, synthetic PREGAP/POSTGAP durations are retained as timeline events, and per-file legacy code-page overrides are available when the CUE text is not valid UTF-8. Cancelled, incomplete or offline scans retain unseen records; a complete scan marks confirmed missing files unavailable without deleting their identities. Recursive directory monitoring coalesces events for 2 seconds, rescans after notification loss, polls every 30 minutes and checks root availability every 60 seconds. Versioned Server browse/search commands provide paginated albums, artists and tracks with cancellation and revision checks.

The control point browses the library on its own pages: an album wall sorted by title, artist, year or date added; artist, composer, genre and year lists that open a filtered album wall; a sortable track list; album details by disc; and Roon-style folder pages that list each library folder by name and location (`\\server\share` or `COMPUTER › [D:] label › folder`), with a breadcrumb and only folders that contain music. Every list is virtualized over pages fetched off the UI thread; each listing's order is built once per library revision, so a random page of 100k tracks takes about 3 ms and of 1,000 albums about 42 ms (p95, Release). Albums are grouped as each scan batch is written, so they appear during a first scan; several library folders scan at the same time, a newly added folder is registered at once, and Settings shows the running scan's progress. Library tracks play and display under their on-disk spelling rather than the lowercase index key. Album-wall tiles, album details, folder and song rows on the Folders page, and the playback bar load covers through the Server off the UI thread. Album covers are resolved from the first track's actual folder (including CD subfolders), then its embedded front cover, then the parent of a disc folder; the playback bar prefers its current track's embedded front cover. A folder uses its own cover, then its first album, then its folder icon. WIC generates aspect-preserving thumbnails up to 256 and 600 pixels, with libwebp when Windows cannot decode WebP. Versioned local JPEGs stay within a 512 MiB disk cache and the UI keeps a 32 MiB thumbnail LRU; obsolete loads are cancelled on recycling, navigation and track changes. Input images are limited to 32 MiB, 16,384 pixels per side and 40 million pixels. A full-width playback bar centers all transport controls with Play / Pause at the window center and keeps only the queue toggle at the right edge. The queue panel opens beside the content (its state is remembered) and supports removing, reordering, clearing, double-click playback and saving as a playlist; tracks and folders offer Play next and Add to end of queue from their row buttons and right-click menu. Search and the Home page follow in later tasks.

M3-R08 adds normalized CJK search and NLS-versioned Windows sorting; affected Library/Server regressions pass 80/80 in Debug and Release, with final text and ordering checks 3/3 in both. OpenCC dictionaries are deployed beside each executable.

Verified moves retain the original file/track identities, ratings, favorites, playback statistics, playlist links and queue entries. Local NTFS/ReFS moves use a volume GUID, 128-bit file ID and creation time; NAS and cross-volume moves require a unique full-file SHA-256 match recorded before the source disappeared. Copies stay separate; without reliable local IDs, missing or ambiguous fingerprints also keep separate identities. Move decisions wait for a complete scan, and partial scans can resume pending decisions. Background fingerprinting checks cancellation and a playback callback between 1 MiB reads, validates file versions before saving and invalidates fingerprints after edits. Wiring this callback to the application playback scheduler remains M3-R06; the library service is still internal.

The WinUI application lives in `src/ControlPoint`, the Server in `src/Server`, the Rust Engine host and decoder adapters in `src/Engine`, the output backends in `src/OutputBridge`, shared types and utilities in `src/Common`, and the CLI in `src/Tools/camelcli`. The Rust workspace in `rust/` holds the Engine crate `rust/audio_engine` and the Symphonia decoder FFI adapter `rust/symphonia_ffi`. Tests are grouped into `tests/server_tests`, `tests/bridge_tests` and `tests/engine_tests`. The Rust Engine now owns the selected FLAC source session, enforces 16/24-bit input, track ranges and seek, and requests decoded frames through a versioned host callback ABI that reuses the four existing adapters. It reports STREAMINFO frame-count and MD5 verification states; full-source MD5 becomes verified only after complete decoding. Render output and the playback state machine are still under development. Tests and all required build configuration are included.

FFmpeg, dr_flac and libsndfile are restored through the pinned vcpkg manifest. Symphonia and the Rust Engine are restored through Cargo with one committed workspace lockfile and linked into one static library. ASIO uses an independent host implementation of the driver interface; no Steinberg SDK is needed to build (ASIO is a trademark of Steinberg Media Technologies GmbH).

The repository also includes third-party license/notice snapshots and a versioned [dependency inventory](third_party/dependencies.json). See [third-party notices](THIRD-PARTY-NOTICES.md) for the source and license information that must accompany a release.

Local planning documents, root-level development probes, private test music, downloaded package directories, build outputs and binary caches are excluded from Git. GitHub's automatic source archive contains the files committed at the release tag; it does not contain dependencies downloaded separately by package managers.

To refresh the checked-in notice snapshots after restoring dependencies:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File msbuild/collect-licenses.ps1
```

## License and release status

CamelPlayer is licensed under the [Mozilla Public License 2.0](LICENSE). Versions published before the change to MPL-2.0 remain available under the GNU GPL version 3. Third-party components retain their own licenses; the root license does not relicense Microsoft binaries or replace LGPL, MPL, MIT, BSD, or other notices.

MPL-2.0 allows CamelPlayer code to be combined with Microsoft-licensed Windows App SDK/WinUI binaries, and no Steinberg ASIO SDK code remains, so no third-party GPL code blocks a binary release. A review of the complete notice set is still a check before the first public release.

For any future binary release, provide access to the exact corresponding source, dependency modifications and build information, and include all required notices. The local packaging command prepares files; it does not establish redistribution permission or publish a release.
