# Changelog

## Unreleased

New since 0.2.0:

- **ADPCM WAV files:** IMA and Microsoft ADPCM, whose 4-bit codes were refused before, play as exact 16-bit PCM, marked as a lossy source. Settings › Decoders has an ADPCM decoder of its own, libsndfile by default. Audio files an earlier version could not read are read again once by the first scan after an update, so ADPCM files already in a library folder appear.

Changed since 0.2.0:

- **Conversion is on by default,** for earlier settings too: when a device does not take a file as it is, CamelPlayer resamples, downmixes, reduces the bit depth or decodes DTS, and the signal path shows it as processed. **Skip it and tell me** in Settings › Playback still plays bit-perfect only.

Fixed since 0.2.0:

- **DTS-CD files played as loud noise.** A WAV file that holds a DTS surround bitstream instead of PCM is now recognized by its content and never played as PCM. With conversion, the chosen WAV decoder reads it, FFmpeg's DTS decoder decodes it and it is downmixed to stereo; without, it is skipped as unsupported.
- **A rate the driver offers but cannot run,** such as 64 kHz on Chord's ASIO driver, ended in a driver error. It now counts as a rate the device does not take: with conversion it is resampled, otherwise skipped as unsupported.

## 0.2.0

Upgrading from 0.1.0: the first start moves the library to a newer format (schema 18) and keeps the old one as `library.db.v17.bak` in `%LOCALAPPDATA%\CamelPlayer`. 0.1.0 refuses the newer library, so to go back to it, close CamelPlayer, delete `library.db`, `library.db-wal` and `library.db-shm`, and rename `library.db.v17.bak` to `library.db`; what changed since the upgrade, such as plays, favorites and ReplayGain analysis, is lost that way. 0.1.0 reads the settings too, but keeps only those it knows, so the DSP, volume and Discord settings go when it saves.

New features since 0.1.0:

- **Parametric EQ** in Settings › DSP. It starts with five bands and the editor adds up to ten, with peaking, shelf, low and high pass (6 to 48 dB per octave), band pass, notch and all-pass filters, each on both channels or one. Drag a point on the response curve to change frequency and gain, and the mouse wheel to change Q. Presets can be saved, Reset returns to the five flat bands, and Equalizer APO and AutoEq files can be imported and exported.
- **Headphone crossfeed** (bs2b) with the Default, Chu Moy and Jan Meier presets or a custom cut-off and feed.
- **Convolution** with an impulse response: a mono, stereo or true-stereo .wav, or a .zip with one per sample rate. A missing rate is converted and cached; responses up to 1,048,576 samples per channel run without latency, and after the last track their end plays out.
- **Software volume:** volume stays fixed by default, for a DAC or amplifier; software volume turns the player bar's volume button into a vertical slider (−80 to 0 dB) with mute, and at 0 dB playback stays bit-perfect.
- **ReplayGain:** Track, Album or Auto (album gain while the queue plays an album in order), with preamps for tracks with and without values and clipping prevention by peak. Values come from the files' tags; tracks without them are analyzed in the background (EBU R128 with libebur128) while ReplayGain is on, queued tracks first, throttled or fast, and the results stay in the library, never in the files.
- **Crossfade** of 1 to 10 seconds, off by default: natural changes, Previous/Next and playing another track or album overlap with an equal-power fade, while an album played in order stays gapless. Pause, stop, seeking and restarting a track act at once. A track in another format is converted to the open one, so nothing reopens mid-track; raw DSD never crossfades.
- **Signal path panel:** the bit-perfect lamp opens a panel above the player bar with the verdict and the route from the file's format to the device's, then one row per step in three stages, source, processing and output, each with the format after it and a colored dot for bit-perfect, lossless conversion, processed or lossy source. A processing stage with no steps says that nothing there changes the signal.
- **The DSP chain:** EQ, crossfeed and convolution work in an order you can change, after one automatic preamp that keeps the whole chain from clipping. The DSP button in the player bar turns each on or off and switches EQ presets. Changes are heard within about a quarter of a second without a gap; while nothing changes the signal, playback stays bit-perfect.
- **Discord:** Settings › Interface can show what's playing on Discord, off by default: the title, the artists, the album on the cover, the progress bar and the signal path's state as a small dot, with the title in your friends list. Pause and stop clear it, and only track changes, seeks and state changes reach Discord. **Also show the cover** sends the file's own cover through an anonymous Cloudflare quick tunnel that runs only while music plays; cloudflared, a pinned release checked against its SHA-256, is downloaded the first time.

Changed since 0.1.0:

- **Search ranks results:** names that match whole come first, then names that start with the search, then names with a word that starts with it, then the rest. Artists always rank; tracks and albums rank when a search matches up to 2,000 tracks, and a broader one lists them by title as before.
- **The Tracks list has column headers:** click Title, Artist, Album or Length to sort by it, and again to reverse it.
- **Play on an album or folder page plays it at once;** the arrow beside it offers playing it next and adding it to the queue.
- **The player bar is two rows high:** the timeline row holds only the timeline and the time, and the badges and the DSP button sit beside the volume, favorite and queue buttons, so nothing moves the timeline. The format line is gone; the signal path panel shows the route. Volume is one button: a lock on it marks fixed volume, and with software volume it opens a vertical slider above it.
- **Settings › DSP is three cards,** Volume and ReplayGain, Crossfade, and DSP processing, and each long note shows its first sentence with Learn more.
- Screen readers find the page titles and the settings cards as headings, and hear Play or Pause and the repeat mode as they are. Repeat is a toggle like Shuffle. Counts show digit grouping (70,715), album titles wrap to two lines, the player bar's cover has a tooltip, and the fields of crossfade and ReplayGain are disabled while those are off.

Fixed since 0.1.0:

- With a theme in Settings › Interface other than the Windows one, the colors the player sets itself (the playback state, the output lamp, the DSP button, the signal path dots, the EQ editor, Home's tiles and chart and an album's formats) followed Windows and could be unreadable. They now follow the app's theme and change with it.

## 0.1.0

New features since 0.0.1:

- **More formats:** ALAC, WAV, AIFF, AAC-LC and MP3 join FLAC, each with its own decoder choice. AAC and MP3 decode to 24-bit (or 16-bit) PCM with encoder delay and padding trimmed for exact length and seeking. Files are recognized by their content, not their extension.
- **DSD:** DSF and DSDIFF files play as native DSD on ASIO or as DoP on every backend, up to DSD512 where the device supports it. DSD can also be converted to PCM at 88.2, 176.4 or 352.8 kHz.
- **Gapless playback** between tracks of the same format.
- **Conversion only with permission:** when a device does not take a file as it is, CamelPlayer skips it by default. It can instead resample (libsoxr), downmix surround to stereo or reduce the bit depth with dither, and it then shows the path as processed.
- **WASAPI shared mode**, and WASAPI following the Windows default device. A device you selected yourself is never silently replaced by another one.
- **Output settings for every backend:** fixed sample rate, pre-decode amount, silence after a format change, device release after pausing, polling buffer, MMCSS task and priority, device period, sample format, ASIO buffer and channels, and Kernel Streaming mode and buffers. The ASIO driver's callback thread joins MMCSS Pro Audio when the driver leaves it at normal priority, and late callbacks are reported in the device details.
- **Signal path display:** badges show bit-perfect, processed, lossy source and raw DSD playback. The format line expands into every processing step. The Settings status area lists the output format, mode, device buffer, underruns, and any requested setting the device replaced.
- **Music library:** add folders on local drives or NAS shares, several at once. Scanning runs in the background and follows changes automatically. Offline folders keep their tracks, and moved files keep their play history, favorites and playlist entries.
- **Library browsing:** a Home page, an album wall sorted by title, artist, year or date added, and artist, composer, genre and year lists. There is also a sortable track list, album details by disc with each album's formats, and folder pages, all with cover art. A folder page plays the whole folder from its Play button and shows each track's number on its album and its file name. A jump bar beside the artist, composer, genre and album lists goes straight to #, A–Z, kana or Han, and album covers are sized so that every row fills the window.
- **Folder tree:** Settings › Interface can show the Folders page as one tree in foobar2000's style, with track counts, file names and the folders you left open.
- **Artist names:** a credit that names several artists, such as "Aimer with chelly" or "A feat. B", counts each of them among the artists and composers, and each name on an album page and in the player bar opens that artist. "&", "," and "/" divide names where at least one part also appears on its own, while bands such as "Bob Marley & The Wailers" keep their names.
- **Artist and composer pages:** the ten most played tracks, the artist's own albums and the albums they appear on, each with View all.
- **Artist pictures** on artist and composer pages and in the Artists and Composers lists: your own picture, a picture in the artist's folder or tracks, or, once you allow it, one from Wikimedia Commons with its author and license. Several artists of the same name can be told apart, and the online lookup can be turned off in Settings.
- **Search** from the title bar (Ctrl+F) with suggestions as you type, including Chinese and Japanese text.
- **Play queue:** play next, add to the end, reorder, remove and save as a playlist, with shuffle, repeat and track history. The queue and position are restored at the next start. Album, folder, track, artist, search, playlist and smart-list pages mark the track it is on, playing or paused.
- **Autoplay:** when the queue ends, random library tracks keep playing.
- **Playlists:** create them from any selection, then rename, reorder, remove entries, and import and export M3U8 files.
- **Favorites** for tracks, albums, artists and composers. The Artists and Composers lists can show only their favorites.
- **Smart lists:** recently added, most played, last played, favorite tracks, favorite albums, favorite artists and never played.
- **Listening page:** listening time and plays for today, this week, this month, this year and all time, with this month's time, plays and most played album on Home. The chosen period shows a listening-time chart, the most played albums, artists and tracks by plays or listening time, and the play history. The statistics of selected tracks can be reset from their right-click menu.
- **Windows integration:** media keys, the volume flyout and the lock screen show and control playback. System-wide hotkeys can be changed, and keyboard shortcuts work while the window is in front. CamelPlayer keeps running in the notification area after its window closes, and only one copy runs at a time.
- **First-run guide** for choosing the look, adding music folders and choosing the output.
- **Light and dark themes**, or the Windows setting, switched at once from Settings or the first-run guide.
- **Settings resets and data:** reset playback, the interface, the library, the cover cache, the listening statistics or everything, and see where data is stored.
- **Automatic updates:** CamelPlayer checks GitHub Releases at startup and daily, and after you confirm it downloads and verifies the new version, installs it and restarts. A failed update is undone.
- **Installation check:** the first start of each version compares the program files with the release's list, and Repair downloads the same version again to replace damaged or missing files.
- **Damaged data recovery:** a damaged settings file comes back from the copy saved at the previous start, and a damaged library from its daily backup; a banner says what was repaired. Data written by a newer version is left untouched, with a message instead of a silent exit.
- **Diagnostic bundle:** Settings › About saves logs, settings, the output devices with the formats they accept, the installation check and the newest crash report in one zip for problem reports. Paths are left out unless you include them.
- **Interface in English and Traditional Chinese.**
- **Portable release:** a zip file with no installer, with its user guide in English and Traditional Chinese, third-party notices, the corresponding third-party source and SHA-256 checksums.
