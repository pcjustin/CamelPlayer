# CamelPlayer user guide

CamelPlayer plays FLAC, ALAC, WAV, AIFF, AAC, MP3 and DSD (DSF and DSDIFF) files on Windows, sending lossless music to your DAC bit-perfect. This guide covers installing it, getting around, choosing how it reaches your DAC, DSD playback and common problems. [繁體中文版](UserGuide.zh-TW.md)

## Installing

1. Download `CamelPlayer-<version>-win-x64.zip` from the project's GitHub Releases page. To check the download, compare its SHA-256 hash (`Get-FileHash` in PowerShell) with the line for it in `CamelPlayer-<version>-SHA256SUMS.txt` from the same release.
2. Extract the whole zip to a folder you can write to, for example `C:\Apps\CamelPlayer`. Keep all files together; nothing needs to be installed.
3. Run `CamelPlayer.exe`. The program is not code-signed, so Windows may show "Windows protected your PC" the first time: choose **More info**, then **Run anyway**.

CamelPlayer needs 64-bit Windows 10 version 1809 or later, or Windows 11. Its settings, library and logs are kept in `%LOCALAPPDATA%\CamelPlayer`; Settings › Reset and data shows the folder and its size. To remove CamelPlayer, delete its folder and, if you want, the data folder.

The first start opens a short guide: choose a light or dark look (or the Windows setting), add your music folders (local drives or a NAS) and choose the output. In the folder window, hold Ctrl to select several folders at once. The library is scanned in the background and albums appear as it goes. You can skip the guide; Settings › Interface opens it again.

Settings › Interface › **Theme** switches between light, dark and the Windows setting at any time.

Closing the window keeps CamelPlayer in the notification area so media keys and hotkeys keep working; use **Exit** in its tray menu to quit, or change this in Settings › Interface.

## Getting around

**Home** shows recently played and recently added albums and, at the bottom, this month's listening time, plays and most played album. Click that card for the **Listening** page, where the cards for today, this week, this month, this year and all time choose the period of the chart, the most played albums, artists and tracks (by plays or listening time) and the play history below them. **View all** opens a whole list. Back in the title bar, Alt+← or the mouse back button returns to Home.

The navigation pane lists **Albums** (sorted by title, artist, year or date added), **Artists**, **Composers**, **Genres**, **Years**, **Tracks** and **Folders**. The letters beside the album, artist, composer and genre lists jump to that part of the list; in a short window a dot stands for the letters left out between two others. An album lists its tracks by disc with a badge for each format, bit depth and sample rate it contains. An artist or composer opens their own page: **Tracks** lists the ten you play most (then by year and album order) with **View all** for every one of them, **Albums** holds the albums whose album artist names them, and **Appears on** the other albums with some of their tracks. The circle beside their name shows their picture, or their initial when there is none: click it for **Choose picture…** (JPEG, PNG or WebP; a picture that is not square keeps its middle, or its top when it is tall) and **Remove picture**. CamelPlayer also uses an `artist.jpg` in a folder named exactly like them or an artist picture inside their tracks. The first artist page asks whether to look pictures up on Wikimedia Commons; only names are sent, and Settings › Interface › **Artist pictures** changes the answer. A Commons picture shows its author and license under the name, linking to the picture's page. When several artists share the name, **Which artist is this?** in the picture's menu picks the right one. Each artist name on an album page, on an artist page and in the player bar opens that artist. A credit that names several artists counts each of them: "Aimer with chelly", "A feat. B" or "A; B" always, and "A & B", "A, B" or "A / B" when at least one of the names also appears on its own in your library. A band such as "Bob Marley & The Wailers" stays one artist, and so does "Yo-Yo Ma, cello". Search from the title bar (Ctrl+F): suggestions appear as you type, and Enter lists every match.

A folder page lists its subfolders, then its own tracks, each with its number on its album and its file name under its title. **Play** at the top plays the whole folder, its subfolders included, or puts it next or at the end of the queue, so there is no need to go back up a level and right-click it.

Settings › Interface › **Folders** shows the Folders page as one tree, as in foobar2000. Each music folder is a top node; click a folder to open or close it, and its tracks appear inside by file name, with each folder's track count after its name. Double-click a folder to play every track below it, or a track to play its folder from there; the arrow keys open and close folders and Enter plays. Ctrl and Shift select several folders and tracks for the right-click menu. The folders you leave open are open again next time.

Click a track to play it. Wherever the track the queue is on is listed (an album, a folder, the track list, an artist page, search results, a playlist or a smart list), a speaker stands in for its number, or a pause sign while it is paused, and its title shows in the accent colour; the mark goes when playback stops. Right-click tracks, albums, artists or folders for **Play next** and **Add to end of queue**; Ctrl-click and Shift-click select several rows first. The queue button at the right of the player bar opens the queue: drag entries to reorder them, press Delete to remove them, and use its ⋯ menu to clear the queue, save it as a playlist or turn **Autoplay when the queue ends** on or off. Autoplay keeps random library tracks you have not played in the last 7 days playing after the last one. The cover in the player bar opens the playing album.

Right-click a selection for **Create playlist** or **Add to playlist**. A playlist plays, renames, reorders by dragging and saves an M3U8 file with **Export M3U8**; **Import M3U8** on the Playlists page reads M3U8 and M3U files. The heart beside a track or album marks it as a favorite; an artist or composer is marked by the heart on their page or from the right-click menu of their name, and **Favorites only** narrows the Artists and Composers lists to them. **Smart Playlists** gathers recently added, most played, last played and never played tracks and your favorites, with **Favorite artists** listing your artists and then your composers.

| Keys | While CamelPlayer is in front |
| --- | --- |
| Space | Play or pause |
| Ctrl+→ / Ctrl+← | Next or previous track |
| Ctrl+Shift+→ / Ctrl+Shift+← | Forward or back 10 seconds |
| Alt+← or the mouse back button | Back |
| Ctrl+F | Search the library |
| Delete | Remove the selected queue or playlist entries |

The system-wide hotkeys (Alt+Home to play or pause, Alt+Page Up and Alt+Page Down for the previous and next track) work in every program and can be changed in Settings › Hotkeys.

## Updates

CamelPlayer asks GitHub for a newer release when it starts and once a day while it runs. Each check is one request to the GitHub Releases API of the project; it sends nothing about you or your library. When a new version is out, a banner at the top of the window offers **Update**, **Release notes** and **Skip this version**; closing the banner means "later". Nothing is downloaded until you click **Update**.

**Update** downloads the release zip, checks it against the release's SHA-256 checksums, then asks to restart if music is playing. CamelPlayer closes and the updater replaces the program files, then starts the new version. Your queue and position come back paused, and the new version shows what changed since your previous version. If the program folder is protected (for example under Program Files), Windows asks for administrator rights. If anything fails, or the new version does not start, the previous version and your settings and library are restored, and that version is no longer offered automatically.

Settings › Interface › **Check for updates automatically** turns the startup and daily checks off. Settings › About › **Check for updates** checks at any time. You can still update by hand: extract the new zip over the old folder or into a new one; your settings and library stay.

## Choosing an output backend

Settings › Playback › Output backend picks how audio reaches the device. Each backend keeps its own device and settings.

| Backend | Use it when | Notes |
| --- | --- | --- |
| WASAPI, exclusive (default) | Almost always | Bit-perfect. "Follow Windows default" moves playback to a new default device; a device you select pauses when it disappears. |
| WASAPI, shared | Other programs must play at the same time | Goes through the Windows mixer, so it is always converted and shown as Processed. |
| ASIO | Your DAC has its own ASIO driver | Bit-perfect and the only backend with native DSD. **Open control panel** shows the driver's own settings. |
| Kernel Streaming | A device without ASIO where WASAPI misbehaves | Talks to the driver directly. While it plays, Windows and other programs cannot use the device. |

The badges at the bottom right of the player bar show the path in use: **Bit-perfect** means the file's samples reach the device unchanged, **Processed** that something converted them (shared mode, a fixed sample rate or a conversion you allowed). Click the format line to see each step. The Status card in Settings lists the output format, the mode, the device buffer and underruns; "Requested ≠ active" means the device could not take a saved setting and what it uses instead.

When a device does not take a file as it is, CamelPlayer skips it and tells you, so nothing is converted without your permission. Set **When the device does not take a file as it is** to **Convert it** to allow resampling, downmixing to stereo or reducing the bit depth. **Sample rate** fixes one output rate for every PCM file (shown as Processed); leave it at **Follow the file** for bit-perfect playback.

## DSD playback

DSF and DSDIFF files are sent to the DAC as raw DSD in one of two ways, set per backend with **DSD output**:

- **Native DSD** (ASIO only, its default): the driver receives DSD directly.
- **DoP** (DSD over PCM, the default for WASAPI and Kernel Streaming): DSD packed into 24-bit PCM frames that a DoP-capable DAC unpacks. DoP needs a PCM rate of DSD rate / 16, so DSD256 needs 705.6 kHz and DSD512 needs 1411.2 kHz; few devices offer the latter.

While raw DSD plays, the **DSD** or **DoP** badge shows that volume and every other PCM processing step are bypassed.

**Convert to PCM** turns DSD into PCM at the **DSD to PCM sample rate** (88.2, 176.4 or 352.8 kHz) for DACs without DSD support. If you keep native or DoP output, **When the device takes neither native DSD nor DoP** decides what happens when the device refuses it: skip the file and tell you (default), or convert it to PCM. DSDIFF files compressed with DST are not supported.

## DSP: equalizer, crossfeed and convolution

Settings › DSP holds three kinds of processing, all off by default: a parametric EQ, headphone crossfeed and convolution with an impulse response. They work in the order shown under **Order**, EQ → crossfeed → convolution by default, so a room or headphone correction sits nearest the speakers or headphones. Use the arrows to move one, and **Restore default order** to go back. The order changes the sound only where left and right are treated differently, such as EQ bands for one channel or a true-stereo response, next to crossfeed.

One preamp covers the whole chain: it is lowered automatically by the largest boost of everything that is on, so nothing clips, and the line at the top of the card shows by how much. Corrections that cancel each other cost no level.

**Parametric EQ.** It starts with five flat bands: a low shelf at 100 Hz, peaking bands at 400 Hz, 1 kHz and 4 kHz, and a high shelf at 10 kHz.

- Drag a numbered point on the curve to change its band's frequency and gain, and turn the mouse wheel over it to change its Q. The rows under the curve take exact values, the filter type (peaking, low or high shelf, low or high pass, band pass, notch or all pass), the channels (both, left or right) and, for low and high pass, a slope of 6 to 48 dB per octave. **Add band** adds bands up to ten, and a band's check box turns it off without removing it.
- **Save as preset…** names the current bands, and the preset list switches between them. **Import…** reads an Equalizer APO configuration or an AutoEq ParametricEQ.txt file as a new preset named after the file, and **Export…** writes the bands in the Equalizer APO format. **Reset** goes back to the five flat bands; saved presets stay.

**Crossfeed** (for headphones) sends some of each channel to the other ear, as speakers do, using bs2b. **Default** is 700 Hz and 4.5 dB, **Chu Moy** 700 Hz and 6 dB, **Jan Meier** 650 Hz and 9.5 dB. **Custom** takes a cut-off of 300–2000 Hz and a feed of 1–15 dB; a lower feed sends more to the other ear.

**Convolution** applies an impulse response, such as a room correction filter. **Choose impulse response…** takes a .wav that is mono (used for both channels), stereo, or 4-channel true stereo (left to left, left to right, right to left, right to right), or a .zip holding one such .wav per sample rate. The rate that plays is taken from the .zip; when it is missing, the nearest rate is converted and cached. A response may have up to 1,048,576 samples per channel at the playing rate. The card shows the file's channels, length and rates.

The **DSP** button in the player bar turns each of the three on or off, switches EQ presets and opens Settings › DSP. It is highlighted while the chain changes the sound.

Changes are heard within about a quarter of a second, without a gap. While the chain acts, the badge shows **Processed**, the signal path lists each step, and the output is dithered once to the device's bit depth. While nothing changes the signal, playback stays bit-perfect. After the last track, the end of a convolution's response plays out. Raw DSD (native or DoP) bypasses the chain; convert DSD to PCM to process it.

## Common problems

**A file is skipped with "The device cannot play this format bit-perfectly".** The device does not accept the file's sample rate, bit depth or channel count. Check that the right device is selected, try a different **Sample format**, or allow conversion (see above).

**No sound, or "Output device not connected".** Turn the DAC on and choose it under Output device. The lists update by themselves when a device appears or goes away; **Refresh device lists** reads them again at once. A device you selected yourself is never replaced by another one; select it again or choose "Follow Windows default".

**DSD plays as loud noise or hiss.** The DAC does not understand DoP (or the driver does not pass it unchanged). Set DSD output to **Convert to PCM**. Turn the volume down before trying DSD on a new device.

**Other programs are silent while CamelPlayer plays.** Exclusive mode, ASIO and Kernel Streaming take the device for themselves. Stop releases it at once; after a pause it is released after **Release the device after pausing** (one second by default). To play alongside other programs, use WASAPI shared mode.

**Clicks, missing beginnings or a short mute when the format changes.** Some DACs need time to switch between sample rates or between PCM and DSD. Raise **Silence after a format change** (Buffers and timing) to 200–500 ms.

**Dropouts or crackles.** The Status card counts underruns. Use a larger buffer: the ASIO buffer size (or the driver's control panel), a longer WASAPI device period or polling buffer, or a longer Kernel Streaming buffer. Keep the MMCSS task at Pro Audio.

**A library folder shows as offline.** The drive or NAS share cannot be reached. Its tracks stay in the library, marked unavailable, and come back when the folder does. New files are found automatically; if tags look out of date, **Rebuild the library** in Settings › Reset and data rereads every file and keeps statistics and playlists.

**"CamelPlayer repaired damaged data".** A settings file or library database that could not be read is kept with the `.bad` extension in the data folder. The settings come back from the copy saved at the previous start. The library comes back from its daily backup, made while CamelPlayer is idle or as it exits, so changes since then may be missing. Without a usable copy the settings return to their defaults, or a new library starts and the music folders have to be added again.

**CamelPlayer says its data was written by a newer version.** An older CamelPlayer was started after a newer one had upgraded the settings or the library. Start the newer version again; the older one changed nothing.

**A hotkey does not work.** Settings › Hotkeys reports a combination another program already holds; press a different one.

**"Some program files are damaged or missing".** The first time each version starts, CamelPlayer compares its program folder with the release's list of files; **Check installation** in Settings › About does it at any time. **Details** lists the files. **Repair** downloads the same version from GitHub again and replaces the program files; your settings and library stay. If an antivirus program quarantined a file, restore it there or exclude the CamelPlayer folder first.

**Windows reports a missing DLL, or CamelPlayer does not start at all.** The zip was not extracted completely, CamelPlayer was started from inside the zip, or an antivirus program removed a file. Extract the whole zip again into a folder of its own and start `CamelPlayer.exe` from there.

## Reporting a problem

Open an issue on the project's GitHub page and fill in the form. In Settings › About, select **Create diagnostic bundle…** and attach the zip it saves. It holds the logs, settings, your output devices with the formats they accept, the result of the installation check, and the newest crash report. File and folder paths are left out unless you check **Include file and folder paths**. The crash report is a memory snapshot of CamelPlayer, so only attach the bundle where you are comfortable sharing it.
