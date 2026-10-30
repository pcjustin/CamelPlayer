# CamelPlayer user guide

CamelPlayer plays FLAC, ALAC, WAV, AIFF, AAC, MP3 and DSD (DSF and DSDIFF) files on Windows, sending lossless music to your DAC bit-perfect. This guide covers installing it, choosing how it reaches your DAC, DSD playback and common problems. [繁體中文版](UserGuide.zh-TW.md)

## Installing

1. Download `CamelPlayer-<version>-win-x64.zip` from the project's GitHub Releases page. To check the download, compare its SHA-256 hash (`Get-FileHash` in PowerShell) with the line for it in `CamelPlayer-<version>-SHA256SUMS.txt` from the same release.
2. Extract the whole zip to a folder you can write to, for example `C:\Apps\CamelPlayer`. Keep all files together; nothing needs to be installed.
3. Run `CamelPlayer.exe`. The program is not code-signed, so Windows may show "Windows protected your PC" the first time: choose **More info**, then **Run anyway**.

CamelPlayer needs 64-bit Windows 10 version 1809 or later, or Windows 11. Its settings, library and logs are kept in `%LOCALAPPDATA%\CamelPlayer`; Settings › Reset and data shows the folder and its size. To remove CamelPlayer, delete its folder and, if you want, the data folder.

The first start opens a short guide: add your music folders (local drives or a NAS) and choose the output. In the folder window, hold Ctrl to select several folders at once. The library is scanned in the background and albums appear as it goes. You can skip the guide; Settings › Interface opens it again.

Closing the window keeps CamelPlayer in the notification area so media keys and hotkeys keep working; use **Exit** in its tray menu to quit, or change this in Settings › Interface.

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

The badges under the playback controls show the path in use: **Bit-perfect** means the file's samples reach the device unchanged, **Processed** that something converted them (shared mode, a fixed sample rate or a conversion you allowed). Click the format line to see each step. The Status card in Settings lists the output format, the mode, the device buffer and underruns; "Requested ≠ active" means the device could not take a saved setting and what it uses instead.

When a device does not take a file as it is, CamelPlayer skips it and tells you, so nothing is converted without your permission. Set **When the device does not take a file as it is** to **Convert it** to allow resampling, downmixing to stereo or reducing the bit depth. **Sample rate** fixes one output rate for every PCM file (shown as Processed); leave it at **Follow the file** for bit-perfect playback.

## DSD playback

DSF and DSDIFF files are sent to the DAC as raw DSD in one of two ways, set per backend with **DSD output**:

- **Native DSD** (ASIO only, its default): the driver receives DSD directly.
- **DoP** (DSD over PCM, the default for WASAPI and Kernel Streaming): DSD packed into 24-bit PCM frames that a DoP-capable DAC unpacks. DoP needs a PCM rate of DSD rate / 16, so DSD256 needs 705.6 kHz and DSD512 needs 1411.2 kHz; few devices offer the latter.

While raw DSD plays, the **DSD** or **DoP** badge shows that volume and every other PCM processing step are bypassed.

**Convert to PCM** turns DSD into PCM at the **DSD to PCM sample rate** (88.2, 176.4 or 352.8 kHz) for DACs without DSD support. If you keep native or DoP output, **When the device takes neither native DSD nor DoP** decides what happens when the device refuses it: skip the file and tell you (default), or convert it to PCM. DSDIFF files compressed with DST are not supported.

## Common problems

**A file is skipped with "The device cannot play this format bit-perfectly".** The device does not accept the file's sample rate, bit depth or channel count. Check that the right device is selected, try a different **Sample format**, or allow conversion (see above).

**No sound, or "Output device not connected".** Turn the DAC on, choose it under Output device and select **Refresh device lists**. A device you selected yourself is never replaced by another one; select it again or choose "Follow Windows default".

**DSD plays as loud noise or hiss.** The DAC does not understand DoP (or the driver does not pass it unchanged). Set DSD output to **Convert to PCM**. Turn the volume down before trying DSD on a new device.

**Other programs are silent while CamelPlayer plays.** Exclusive mode, ASIO and Kernel Streaming take the device for themselves. Use WASAPI shared mode, or set **Release the device after pausing** to a short time.

**Clicks, missing beginnings or a short mute when the format changes.** Some DACs need time to switch between sample rates or between PCM and DSD. Raise **Silence after a format change** (Buffers and timing) to 200–500 ms.

**Dropouts or crackles.** The Status card counts underruns. Use a larger buffer: the ASIO buffer size (or the driver's control panel), a longer WASAPI device period or polling buffer, or a longer Kernel Streaming buffer. Keep the MMCSS task at Pro Audio.

**A library folder shows as offline.** The drive or NAS share cannot be reached. Its tracks stay in the library, marked unavailable, and come back when the folder does. New files are found automatically; if tags look out of date, **Rebuild the library** in Settings › Reset and data rereads every file and keeps statistics and playlists.

**A hotkey does not work.** Settings › Hotkeys reports a combination another program already holds; press a different one.

## Reporting a problem

Open an issue on the project's GitHub page and fill in the form. In Settings › About, select **Create diagnostic bundle…** and attach the zip it saves. It holds the logs, settings, your output devices with the formats they accept, and the newest crash report. File and folder paths are left out unless you check **Include file and folder paths**. The crash report is a memory snapshot of CamelPlayer, so only attach the bundle where you are comfortable sharing it.
