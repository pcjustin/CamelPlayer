# Validation

This file records how CamelPlayer 0.1.0 was checked beyond its ordinary automated tests: on audio hardware, on a NAS and for speed. Each section names when its run took place; later changes may have moved the numbers. [README.md](README.md#testing) explains how to run the tests.

## Automated tests

On 2026-10-30 a full Release run passed all 638 enabled C++ tests, including the 100k-track search latency target, and all 47 Rust Engine tests. 112 tests are disabled in ordinary runs because they play audio on named devices or scan a NAS.

Earlier targeted runs:

- Artwork regressions passed 14/14, including covers in CD subfolders and images with misleading extensions, and every cover displayed correctly in the app.
- Library identity and playback-gating regressions passed 137/137 in Debug.
- Search and sorting regressions passed 80/80 in Debug and Release, and the final text and ordering checks 3/3 in both.
- Three Server concurrency and listener tests passed 20 repetitions each (60 runs).
- The NAS follow-up changes passed their 30 related tests in Debug and Release.
- A full Debug run passed 562 of 563: the 100k-track search latency target (p95 under 100 ms) applies to optimized builds and measured 332 ms unoptimized.

What the decoding tests check:

- Server capture tests compare 16- and 24-bit PCM from all four FLAC decoders with an independent libFLAC decode.
- Complete decodes from frame zero verify an available STREAMINFO MD5 and enforce known frame counts; after a seek, the remaining frame count is checked instead of comparing a partial digest with the whole-file MD5.
- Generated 20- and 32-bit FLAC files check that unsupported depths are refused cleanly.
- MP3 test files are encoded with LAME inside the test program only.

## Hardware

The hardware runs of 2026-10-20 and 2026-10-21 played through the Engine and Server APIs on a Chord DAVE (WASAPI, ASIO and Kernel Streaming) and a built-in Cirrus codec (WASAPI and Kernel Streaming).

- 91 hardware cases passed: FLAC playback controls on every backend of both devices, and WAV, ALAC, AAC, MP3, WASAPI shared mode and DSD (native, DoP and DSD to PCM) wherever the device can play them.
- DSD64 to DSD512 played on the DAVE. DSD512 over DoP needs a 1411.2 kHz carrier; where the device does not offer it, the attempt is refused or reported as a driver failure rather than assumed to work.
- Kernel Streaming has no native DSD, because Windows defines no Kernel Streaming DSD format, so it plays DSD as DoP or, when allowed, as PCM.
- The Cirrus codec takes 24-in-32-bit samples up to 192 kHz, a container the DAVE refuses. It has no DSD mode, so it receives DSD only converted to PCM, never DoP or native DSD.
- A separate DAVE run of the output parameters (polling buffer, MMCSS, fixed sample rate, silence after a DoP-to-PCM switch and release after a pause) passed 5/5.

## Library performance

Measured on 2026-10-24 after the scan speed-up, in Release with caches not flushed:

- The first page of a grouped search over 100k tracks: p95 79.5–85.4 ms, with the cache warm.
- A first scan of 100k synthetic tracks on a local SSD: 364 s (target: 15 minutes or less).
- A read-only full scan of an 86,684-file NAS share (75,507 tracks): 27.1 minutes; unchanged rescans took 19–20 seconds. NAS scan time is recorded rather than targeted.

The run before the speed-up, earlier the same day, measured a 100k-track search first page at p95 85.4 ms and one- and two-character fallback searches at p95 1,146 and 939 ms. Its synthetic first scan took 897.3 s, with the first 500-file batch at 633.3 s; the same NAS share took 10,507 s to scan fully (147 diagnostics) and 192.7 s to rescan unchanged (no updates, 4 diagnostics). Fingerprinting then hashed whole files: a run over that share was stopped after 157 of 86,680 files (3.21 GB) in 438 s, a rate that projected about 109 hours for its 2.87 TB, and the local index passed the SQLite integrity check afterwards. Fingerprints have since been sampled from the first and last 64 KiB of each file.

## NAS

A Release run on a real SMB share on 2026-10-23 and 2026-10-24 passed:

- full and unchanged incremental scans of representative FLAC and DSD folders;
- 2,000 change events, with a zero-byte notification-overflow signal;
- recursive rename and delete detection;
- a temporary folder becoming unavailable and reconnecting, without losing records.

## Known limits

- In that NAS environment, tag edits did not reach the index through change notifications within 120 seconds; a test-only 2-second polling interval recovered them. Network shares are polled every 6 hours and local folders every 30 minutes, so a change that notifications miss can stay stale until the next poll.
- A physical network interruption during a scan remains untested.
