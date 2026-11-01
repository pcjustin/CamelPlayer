# Validation

This file records how CamelPlayer 0.1.0, 0.2.0 and 0.2.1 were checked beyond their ordinary automated tests: on audio hardware, in the app, on a NAS and for speed. Each section names when its run took place; later changes may have moved the numbers. The library performance and NAS runs were not repeated for 0.2.0 or 0.2.1. [README.md](README.md#testing) explains how to run the tests.

## Automated tests

On 2026-11-01 and 2026-11-02, for 0.2.1, every C++ project was rebuilt from scratch in Release and Debug. A full Release run passed all 731 enabled C++ tests, including the 100k-track search latency target, and all 88 Rust Engine tests. A full Debug run passed 730 of 731, the latency test again failing only unoptimized (first page p95 102.6 ms, broad suggestions 481 ms). After the last change, a WASAPI fix that no automated test reaches, a second Release run passed 730: the test of a manual change during a crossfade reopened the device once under the full suite's load and passed 10 times out of 10 alone.

On 2026-11-01, for 0.2.0, a full Release run passed all 715 enabled C++ tests, including the 100k-track search latency target, and all 88 Rust Engine tests. A full Debug run passed 714 of 715: unoptimized, the latency test measured a first-page p95 of 102.9 ms and a broad-suggestion p95 of 483 ms against its 100 ms target for optimized builds. 112 tests are disabled in ordinary runs because they play audio on named devices or scan a NAS.

On 2026-10-31, for 0.1.0, a full Release run passed all 661 enabled C++ tests, including the latency target, and all 47 Rust Engine tests.

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

On 2026-11-02, before 0.2.1, all 97 hardware tests passed twice, in 189 and 190 seconds. Before that the WASAPI polling test had failed in 2 of 5 runs right after other tests switched the DAVE to another format: the driver reported room that GetBuffer did not grant yet, which 0.2.1 waits out. Five runs of the output parameter tests in a row then passed.

On 2026-11-01, before 0.2.0, all 97 hardware tests passed again in 195 seconds on the same two devices, as they had on 2026-10-31 before 0.1.0 in 196 seconds: playback on every backend with each FLAC decoder, the other formats, DSD rates and DSD switching, device fallback, the output parameters and the Server's playback path.

The hardware runs of 2026-10-20 and 2026-10-21 played through the Engine and Server APIs on a Chord DAVE (WASAPI, ASIO and Kernel Streaming) and a built-in Cirrus codec (WASAPI and Kernel Streaming).

- 91 hardware cases passed: FLAC playback controls on every backend of both devices, and WAV, ALAC, AAC, MP3, WASAPI shared mode and DSD (native, DoP and DSD to PCM) wherever the device can play them.
- DSD64 to DSD512 played on the DAVE. DSD512 over DoP needs a 1411.2 kHz carrier; where the device does not offer it, the attempt is refused or reported as a driver failure rather than assumed to work.
- Kernel Streaming has no native DSD, because Windows defines no Kernel Streaming DSD format, so it plays DSD as DoP or, when allowed, as PCM.
- The Cirrus codec takes 24-in-32-bit samples up to 192 kHz, a container the DAVE refuses. It has no DSD mode, so it receives DSD only converted to PCM, never DoP or native DSD.
- A separate DAVE run of the output parameters (polling buffer, MMCSS, fixed sample rate, silence after a DoP-to-PCM switch and release after a pause) passed 5/5.

## In the app

Checked by hand on 2026-11-02 in the 0.2.1 Release build, on the owner's library and settings (ASIO on the Chord DAVE), which were restored afterwards:

- The first scan by 0.2.1 read again, once, the audio files earlier versions had refused, and the ADPCM WAV files among them became tracks.
- A DTS-CD album (DTS-ES 6.1 in WAV) played through the chosen WAV decoder and FFmpeg's DTS decoder, downmixed from 7 to 2 channels and shown as processed. With conversion switched off, every track was skipped as a format the device cannot play bit-perfectly.
- A 16-bit 64 kHz FLAC that the DAVE's ASIO driver accepts but cannot start played at 96 kHz after the failed start, shown as resampled; without conversion it was reported unsupported.
- IMA ADPCM WAV files played with conversion off as a lossy source, padded from 16 to 32 bits without dither, with libsndfile, FFmpeg and dr_wav in turn.

Checked by hand on 2026-11-01 in a Release build, for 0.2.0:

- On the Chord DAVE, with ReplayGain, the parametric EQ, software volume, a 3-second crossfade and TPDF dither to 24 bits all on, the signal path listed every stage: ReplayGain −8.9 dB, the EQ, volume −6.0 dB, the crossfade and the dither.
- Skipping to the next track crossfaded into it without reopening the device.
- A DSD256 file played as native DSD showed Bit-perfect with nothing changing the signal, and noted that native DSD bypasses the DSP.
- With Discord Rich Presence and its cover on, the owner's Discord profile showed the playing title, artists, album, progress and the Bit-perfect state over the file's own cover, served through the cloudflared tunnel; the friends list showed the title, and pausing cleared the activity. cloudflared was downloaded on first use, checked against its pinned SHA-256 before it ran, and ended with CamelPlayer.

## Library performance

Measured on 2026-10-31 in Release on a copy of the owner's 70,715-track NAS library, with measurements on (`CAMELPLAYER_MEASURE`):

- A warm start showed the main window 1.22–1.25 s after the process started and Home's content after 1.9–2.1 s (target: an interactive main window within 2 seconds). The first start after the copy took 2.9 s, as it also upgraded the library database.
- After fast album-wall scrolling and 30 idle seconds the process used 282 MB of private memory (317 MB working set; target: 500 MB or less).
- During fast album-wall scrolling the frame clock ran at about 32 frames a second on that machine (median 31 ms between frames). Most two-second windows had no frame over 46 ms; a few had one frame of 106–226 ms while covers loaded.
- The owner's 157 plays from the NAS on 2026-10-30 and 2026-10-31 started sound a median 0.53 s after the play request, 90% within 0.98 s and the slowest after 3.0 s (target: 1 second from a NAS). Start-up from local files was not measured again.
- The 100k-track first scan below was not repeated, for lack of free disk space for the synthetic files.

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
