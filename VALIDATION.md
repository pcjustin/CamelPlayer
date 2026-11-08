# Validation

This file records how CamelPlayer 0.1.0, 0.2.0, 0.2.1, 0.2.2, 0.2.3 and 0.3.0 were checked beyond their ordinary automated tests: on audio hardware, on network players, in the app, on a NAS and for speed. Each section names when its run took place; later changes may have moved the numbers. The library performance and NAS runs were not repeated for 0.2.0, 0.2.1, 0.2.2 or 0.3.0. [README.md](README.md#testing) explains how to run the tests.

## Automated tests

On 2026-11-08, for 0.3.0, a full Release run of the release build passed all 904 enabled C++ tests, the 100k-track search latency target included, and all 95 Rust Engine tests passed. The whole Rust workspace passed 826 tests; the one left out is a measurement of the DSD remodulator, run by hand. No Debug run was made for 0.3.0.

The package was checked the same day: its three files matched their SHA-256 sums, the 250 installed files matched `install-manifest.txt`, the program reports version 0.3.0 and is the build the tests ran on, and no PDB, sanitizer runtime, test or FFmpeg file is in it. Started on an empty data folder and closed through the tray at 6–20 s, it exited cleanly every time. Before the fix to exiting, 3 of 8 such runs crashed on exit.

## Sanitizers

From 0.3.0 on, every release runs `build.ps1 -Sanitizers` before it is published. On 2026-11-08, for 0.3.0, on `cf62206` with VERSION at 0.3.0, the code of the release, all 8 checks passed in 4,820 s:

- `/analyze` on the four files with annotated locks;
- Loom on the Engine's ring, and Miri on the DTS C ABI and the ring;
- the C++ suite under AddressSanitizer, 902 tests: the 100k-track latency target and the whole-library queue's time limit are left out, as an instrumented build does not meet them and the Release suite holds them;
- the C++ suite under UndefinedBehaviorSanitizer, all 904 tests;
- 600 s each of fuzzing: the DTS frame reader 68,091 runs, the format probe 67,995 and the network messages 211,670, none failing.

Two earlier runs that day also passed all 8: on `52449ef`, before Symphonia left FLAC covers out, in 4,884 s, and on `c245561`, before the three fixes ahead of that; `/analyze` had found a local variable shadowing another in `LocalServer.cpp`, renamed in `c245561`. A first run before that stopped in the AddressSanitizer step when the disk filled.

## Hardware

On 2026-11-08, before 0.3.0, all 97 hardware tests passed twice on the release build, in 184 and 183 seconds, on the Chord DAVE and the built-in Cirrus codec; the network-player case read the DMP-A6 and the Essential Bravo as UPnP AV and sent them nothing.

On 2026-11-05, before 0.2.3, all 97 hardware tests passed twice on the release build, in 200 and 196 seconds.

On 2026-11-03, before 0.2.2, all 97 hardware tests passed twice on the release candidate, in 194 and 189 seconds, and once more in 194 seconds after manual track changes began to keep the device open.

On 2026-11-02, before 0.2.1, all 97 hardware tests passed twice, in 189 and 190 seconds. Before that the WASAPI polling test had failed in 2 of 5 runs right after other tests switched the DAVE to another format: the driver reported room that GetBuffer did not grant yet, which 0.2.1 waits out. Five runs of the output parameter tests in a row then passed.

On 2026-11-01, before 0.2.0, all 97 hardware tests passed again in 195 seconds on the same two devices, as they had on 2026-10-31 before 0.1.0 in 196 seconds: playback on every backend with each FLAC decoder, the other formats, DSD rates and DSD switching, device fallback, the output parameters and the Server's playback path.

The hardware runs of 2026-10-20 and 2026-10-21 played through the Engine and Server APIs on a Chord DAVE (WASAPI, ASIO and Kernel Streaming) and a built-in Cirrus codec (WASAPI and Kernel Streaming).

- 91 hardware cases passed: FLAC playback controls on every backend of both devices, and WAV, ALAC, AAC, MP3, WASAPI shared mode and DSD (native, DoP and DSD to PCM) wherever the device can play them.
- DSD64 to DSD512 played on the DAVE. DSD512 over DoP needs a 1411.2 kHz carrier; where the device does not offer it, the attempt is refused or reported as a driver failure rather than assumed to work.
- Kernel Streaming has no native DSD, because Windows defines no Kernel Streaming DSD format, so it plays DSD as DoP or, when allowed, as PCM.
- The Cirrus codec takes 24-in-32-bit samples up to 192 kHz, a container the DAVE refuses. It has no DSD mode, so it receives DSD only converted to PCM, never DoP or native DSD.
- A separate DAVE run of the output parameters (polling buffer, MMCSS, fixed sample rate, silence after a DoP-to-PCM switch and release after a pause) passed 5/5.

## Network players and AirPlay receivers

Checked on 2026-11-07 and 2026-11-08 for 0.3.0 with the owner at home, in Release builds started from Explorer on a copy of the owner's library and settings, music on a NAS over Wi-Fi. The players:

- **Eversolo DMP-A6**, wired: UPnP AV, no DSD among its 392 protocolInfo entries, so DSD goes to it as DoP in WAV. Over AirPlay (`et=0,1`, `vs=105.1`, `pw=false`) it takes an unencrypted session.
- **Essential Bravo ID E620** (the owner's KECES, a Linkplay MUZO Cobblestone module, firmware `p20.Linkplay.4.6.529755`): UPnP AV. Its AirPlay receiver (`et=0,4`, `vs=366.0`) answers OPTIONS and refuses ANNOUNCE with 403, so it is listed as not supported and stays on UPnP.
- **Chord Mojo 2 with Poly**: UPnP AV, declaring DSF and DSD.
- **HomePod mini stereo pair** (two `AudioAccessory5,1`, `et=0,3,5`, `vs=1005.13.2`, `ov=27.2`, both announcing the pair's name in `gpn`): AirPlay 1 at 44.1 kHz / 16 bits, a session to each speaker.
- **MacBook Air** (macOS 26, `et=0,3,5`): refuses the connection with 403 and is listed as not supported.

Over UPnP, each of the three players played FLAC from the NAS with seeks and gapless changes, no gap or noise heard; a DSP change restarted the stream after a short pause; a pause from the player's own remote or app was followed at the same position; another source taking the player stopped CamelPlayer with "in use by another source" while the other source played on. DSD64 went to the DMP-A6 as DoP, shown as DSD64 on the player, to the Poly as native DSF, bit-perfect, and to the Bravo as 176.4 kHz PCM, its setting; all three were heard. The DMP-A6's volume was read and followed (32); the Poly's read 100 and was never written. The Bravo, set to fixed volume by JPLAY, reports volume 100 while playing and 0 with mute while paused or in standby; CamelPlayer showed each as read and wrote none. In its standby the Bravo answers Play and then stays stopped, and CamelPlayer reports "did not start playing" 5 s later. Play after CamelPlayer released a player at 60 s paused went on from the same place.

The HomePod pair played from the NAS with a seek, a pause, a track change, a DSP change, the receiver's own volume and a language change that did not stop the music, both speakers heard and no noise; an iPhone taking it over stopped CamelPlayer with "in use by another source". It then played 2 hours and 3 minutes unbroken, 27 CD-format tracks in order, "bit-perfect to the receiver" throughout and no AirPlay warning, error or resend in the log, and the owner heard the two speakers still in step afterwards. Separated in the Home app, one HomePod alone was found and played. Paired again, the pair came back under a new device ID; on 2026-11-08 the release build found it and moved the saved pair's entry, enabled state and volume (29) to the new ID, recording its two speakers, and the old entry went.

## In the app

Checked by hand on 2026-11-07 and 2026-11-08 in Release builds for 0.3.0, on a copy of the owner's library and settings (ASIO on the Chord DAVE, music on a NAS over Wi-Fi):

- The first sound on the DAVE over ASIO from the NAS, warm, with 0%, 50% and 100% of the cores kept busy, from the press to the sound the driver took: the first play 0.21–0.62 s; a change to a track of the same format 0.1–0.6 s, once 1.2 s, to 96 or 192 kHz 0.9–1.4 s, about 450 ms of it reopening the ASIO driver at the new rate, and to DSD64 1.8–2.4 s, the DAVE switching to DSD. A first reading of 1.4–2.2 s for the first play took the time of the engine's log line, written up to a second after the sound. One album's first play still took 1.5–2.0 s: Symphonia read its 3.3 MB embedded cover before the audio, which 0.3.0 now leaves out; on discs of that album not yet read, with the queue panel closed and half the cores busy, the first play took 0.27–0.34 s against 0.31–0.68 s before.
- A DSD256 tone limited to DSD64 on the DAVE showed DSD256 → DSD64, native DSD, processed, in the signal path.
- With "when playing from a folder" on to the end of its library folder, playing a folder from its page and from the tree queued its 16 tracks and then the folders after it, 55,583 entries in all; with it off, the 16 tracks only.
- Go to playing, with 残夢 playing: the album wall went to its card in about a second, the 71,437-track list selected the track in 1.8 s and the closed tree opened down to it in 0.6 s.
- Measuring the load of playback with that 55,583-entry queue found the check of the queue's files reading the whole file table every 5 seconds while a track played, 26 MB each time: the queue position saved every 5 seconds counted as a library change. After the fix, it reads again only when files or library folders change, and once a minute; reads while playing fell from 28 MB to about 2.5 MB per 5 s.
- The first complete scan by 0.3.0 reads every MP3 and every file an earlier version refused again, 3,744 files there: it took 414 s, against about 30 s for an unchanged scan. Closed at 212 s, the next start read them all again in 400 s, as only the end of the scan recorded the version. After the fix, closed at 210 s with 2,008 of the 3,744 done, the next start finished in 228 s.

Checked by hand on 2026-11-05 in the 0.2.3 Release build, on a copy of the owner's library and settings (ASIO on the Chord DAVE, music on a NAS over Wi-Fi):

- FLAC 16/44.1 from the NAS started 326 and 536 ms after the play request and changed gaplessly to the next track. An MP3 with a Xing header started within a second, reading 1.4 MB from the NAS for its 3.9 MB, continued gaplessly into the next track, and a seek to about 70% of that one continued gaplessly into the track after it.
- Measuring the load of playback found the fingerprint pass reading every file row of the library about every 5 seconds while a track played, 43 MB each time: the queue position saved every 5 seconds counted as a library change, and a pass that playback interrupted never counted as done. FLAC playback read 605 MB in a minute at 19.6% of a core. After the fix it read 7.8 MB, its own bit rate, at 5.4% of a core with the window open and 1.7% minimized, with 194 MB of private memory.

Checked by hand on 2026-11-03 in the 0.2.2 Release build, on a copy of the owner's library and settings (ASIO on the Chord DAVE, music on a NAS over Wi-Fi); the owner's own data was not touched:

- Settings › Decoders lists only FLAC, ALAC, WAV and AIFF. Settings saved with minimp3 for MP3, FFmpeg for AAC, dr_wav for ADPCM and 16-bit lossy output played with mpg123, Symphonia and libsndfile, and were saved without the removed choices.
- Files 0.2.1 refused played: MP3s behind stacked ID3 tags (a Mabinogi soundtrack) with mpg123, and an MP4 concert video whose first track is the picture with Symphonia. AAC and ALAC M4A files played with Symphonia, ALAC bit-perfect.
- MP3s from the NAS started 0.6–0.9 s after the play request with mpg123 once the library scan that follows the start had ended, including Chronicle 2nd 01 and Musica 02, whose start had taken 9.4 and 5.2 s in 0.2.1. A seek to about 70% of a track continued gaplessly into the next. Swallow and GAUZE, which Symphonia and dr_mp3 had played wrongly, played with mpg123.
- Changing tracks by hand every 6–8 s, while the playing MP3 was still read from the NAS, first took 1.4–6.7 s: the Server asked the NAS about every queued track and how the chosen file was spelled, and closing and reopening the ASIO driver took about 450 ms. With both fixed, changes took 44–452 ms from the play request.
- Suggestions for broad queries took 37–44 ms; the first query after start, during the startup scan, took 112 ms.

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

On 2026-11-05, for 0.2.3, a track played from the NAS on the Chord DAVE over ASIO while Settings › Rebuild library reread a copy of the owner's 86,479-file library from it. No underrun occurred, and the median start, seek, track change, pause, play, queue, settings, album wall and search suggestion took 47, 1,199, 836, 467, 15, 15, 5, 20 and 47 ms, against 63, 1,220, 885, 464, 15, 15, 12, 27 and 51 ms with the library idle. The rebuild read 1,201 files in 162 s while playing. The share answered slowly that night: the app's startup scan took 374.5 s at 00:45 and 28–63 s after 06:30, against 32–33 s on 2026-11-03.

A Release run on a real SMB share on 2026-10-23 and 2026-10-24 passed:

- full and unchanged incremental scans of representative FLAC and DSD folders;
- 2,000 change events, with a zero-byte notification-overflow signal;
- recursive rename and delete detection;
- a temporary folder becoming unavailable and reconnecting, without losing records.

## Known limits

- In that NAS environment, tag edits did not reach the index through change notifications within 120 seconds; a test-only 2-second polling interval recovered them. Network shares are polled every 6 hours and local folders every 30 minutes, so a change that notifications miss can stay stale until the next poll.
- A physical network interruption during a scan remains untested.
- On this computer's Wi-Fi the NAS gives 0.7–2.3 MB/s, and DSD256 needs 2.82 MB/s, so DSD256 from the NAS breaks up on any output, as DoP or converted to PCM; DSD128 fits.
- While the DMP-A6 plays, the log records "network player volume: HTTP request failed (12152)" now and then.
- In English, the playback bar has no room for the state badge beside "Processed".
