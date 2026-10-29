# Changelog

## 0.1.0

New features since 0.0.1:

- **More formats:** ALAC, WAV, AIFF, AAC-LC and MP3 join FLAC, each with its own decoder choice. AAC and MP3 decode to 24-bit (or 16-bit) PCM with encoder delay and padding trimmed for exact length and seeking. Files are recognized by their content, not their extension.
- **DSD:** DSF and DSDIFF files play as native DSD on ASIO or as DoP on every backend, up to DSD512 where the device supports it. DSD can also be converted to PCM at 88.2, 176.4 or 352.8 kHz.
- **Gapless playback** between tracks of the same format.
- **Conversion only with permission:** when a device does not take a file as it is, CamelPlayer skips it by default. It can instead resample (libsoxr), downmix surround to stereo or reduce the bit depth with dither, and it then shows the path as processed.
- **WASAPI shared mode**, and WASAPI following the Windows default device. A device you selected yourself is never silently replaced by another one.
- **Output settings for every backend:** fixed sample rate, pre-decode amount, silence after a format change, device release after pausing, polling buffer, MMCSS task and priority, device period, sample format, ASIO buffer and channels, and Kernel Streaming mode and buffers.
- **Signal path display:** badges show bit-perfect, processed, lossy source and raw DSD playback. The format line expands into every processing step. The Settings status area lists the output format, mode, device buffer, underruns, and any requested setting the device replaced.
- **Music library:** add folders on local drives or NAS shares. Scanning runs in the background and follows changes automatically. Offline folders keep their tracks, and moved files keep their play history, favorites and playlist entries.
- **Library browsing:** a Home page, an album wall sorted by title, artist, year or date added, and artist, composer, genre and year lists. There is also a sortable track list, album details by disc with each album's formats, and folder pages, all with cover art.
- **Search** from the navigation pane (Ctrl+F) with suggestions as you type, including Chinese and Japanese text.
- **Play queue:** play next, add to the end, reorder, remove and save as a playlist, with shuffle, repeat and track history. The queue and position are restored at the next start.
- **Autoplay:** when the queue ends, random library tracks keep playing.
- **Playlists:** create them from any selection, then rename, reorder, remove entries, and import and export M3U8 files.
- **Favorites** for tracks and albums.
- **Smart lists:** recently added, most played, last played, favorite tracks, favorite albums and never played.
- **Listening statistics on Home:** listening time and plays for today, this week, this month, this year and all time. The chosen period shows a listening-time chart, the most played albums, artists and tracks by plays or listening time, and the play history. The statistics of selected tracks can be reset from their right-click menu.
- **Windows integration:** media keys, the volume flyout and the lock screen show and control playback. System-wide hotkeys can be changed, and keyboard shortcuts work while the window is in front. CamelPlayer keeps running in the notification area after its window closes, and only one copy runs at a time.
- **First-run guide** for adding music folders and choosing the output.
- **Settings resets and data:** reset playback, the interface, the library, the cover cache, the listening statistics or everything, and see where data is stored.
- **Automatic updates:** CamelPlayer checks GitHub Releases at startup and daily, and after you confirm it downloads and verifies the new version, installs it and restarts. A failed update is undone.
- **Diagnostic bundle:** Settings › About saves logs, settings, the output devices with the formats they accept, and the newest crash report in one zip for problem reports. Paths are left out unless you include them.
- **Interface in English and Traditional Chinese.**
- **Portable release:** a zip file with no installer, with its user guide in English and Traditional Chinese, third-party notices, the corresponding third-party source and SHA-256 checksums.
