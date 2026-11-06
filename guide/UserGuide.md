# CamelPlayer user guide

CamelPlayer plays FLAC, ALAC, WAV (including ADPCM and DTS-CD), AIFF, AAC, MP3 and DSD (DSF and DSDIFF) files on Windows, sending lossless music to your DAC bit-perfect. This guide covers installing it, getting around, choosing how it reaches your DAC, DSD playback and common problems. [繁體中文版](UserGuide.zh-TW.md)

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

The navigation pane lists **Albums** (sorted by title, artist, year or date added), **Artists**, **Composers**, **Genres**, **Years**, **Tracks** and **Folders**. The letters beside the album, artist, composer and genre lists jump to that part of the list; in a short window a dot stands for the letters left out between two others. An album lists its tracks by disc with a badge for each format, bit depth and sample rate it contains. An artist or composer opens their own page: **Tracks** lists the ten you play most (then by year and album order) with **View all** for every one of them, **Albums** holds the albums whose album artist names them, and **Appears on** the other albums with some of their tracks. The circle beside their name shows their picture, or their initial when there is none: click it for **Choose picture…** (JPEG, PNG or WebP; a picture that is not square keeps its middle, or its top when it is tall) and **Remove picture**. CamelPlayer also uses an `artist.jpg` in a folder named exactly like them or an artist picture inside their tracks. The first artist page asks whether to look pictures up on Wikimedia Commons; only names are sent, and Settings › Interface › **Artist pictures** changes the answer. A Commons picture shows its author and license under the name, linking to the picture's page. When several artists share the name, **Which artist is this?** in the picture's menu picks the right one. Each artist name on an album page, on an artist page and in the player bar opens that artist. A credit that names several artists counts each of them: "Aimer with chelly", "A feat. B" or "A; B" always, and "A & B", "A, B" or "A / B" when at least one of the names also appears on its own in your library. A band such as "Bob Marley & The Wailers" stays one artist, and so does "Yo-Yo Ma, cello". Search from the title bar (Ctrl+F): suggestions appear as you type, and Enter lists every match. Names that match your search whole come first, then names that start with it, then names with a word that starts with it; a search that matches more than 2,000 tracks lists its tracks and albums by title. On **Tracks**, click a column header to sort by that column, and click it again to reverse the order. To resize the title, artist and album columns, drag the divider between their headers. This works on Tracks, in search results, on an artist's page, on an album with several artists and in a playlist, and every list follows the new widths. Double-click a divider, or reset the interface in Settings › Reset and data, to restore the original widths. Text too long for its column ends in "…", and the full text appears when you rest the pointer on it.

A folder page lists its subfolders, then its own tracks, each with its number on its album and its file name under its title. **Play** at the top plays the whole folder, its subfolders included, or puts it next or at the end of the queue, so there is no need to go back up a level and right-click it. Clicking a track plays its folder from that track to the folder's last track. To keep going, turn on Settings › Playback › **When a track plays from a folder**. Playback then continues through the folders that follow, in the order the Folders page lists them, and moves up through the folders above until it reaches the end of the music folder; for example, an album's DISC 01 goes on to DISC 02. The track starts at once, and the rest join the queue a few seconds later. The folder tree works the same way.

Settings › Interface › **Folders** shows the Folders page as one tree, as in foobar2000. Each music folder is a top node; click a folder to open or close it, and its tracks appear inside by file name, with each folder's track count after its name. Double-click a folder to play every track below it, or a track to play its folder from there; the arrow keys open and close folders and Enter plays. Ctrl and Shift select several folders and tracks for the right-click menu. The folders you leave open are open again next time.

Click a track to play it. On an album or folder page, **Play** plays all of it; the arrow beside it plays it next or adds it to the end of the queue. Wherever the track the queue is on is listed (an album, a folder, the track list, an artist page, search results, a playlist or a smart list), a speaker stands in for its number, or a pause sign while it is paused, and its title shows in the accent colour; the mark goes when playback stops. Right-click tracks, albums, artists or folders for **Play next** and **Add to end of queue**; Ctrl-click and Shift-click select several rows first. The queue button at the right of the player bar opens the queue: drag entries to reorder them, press Delete to remove them, and use its ⋯ menu to clear the queue, save it as a playlist or turn **Autoplay when the queue ends** on or off. Autoplay keeps random library tracks you have not played in the last 7 days playing after the last one. The cover in the player bar opens the playing album.

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

## Choosing an output

The output button in the player bar lists your outputs. Choose one to switch to it: a playing track goes on from where you hear it, with the queue unchanged, and a paused track stays paused.

Settings › Audio outputs lists every device CamelPlayer finds, by backend. The switch at the end of a row shows that output in the menu, the pencil names it as you like, and the bin clears what CamelPlayer saved for it. The same DAC through WASAPI and through ASIO is two outputs. A saved device that is off stays listed as Not connected.

Each output keeps its own settings, shown in Settings › Playback for the output in use; an output you choose for the first time starts from the defaults. "Windows default device (WASAPI)" follows Windows and uses the settings of whichever device is the default.

UPnP AV and OpenHome players on your network appear under Network players. Turning the first one on explains why Windows Firewall has to let them reach CamelPlayer and asks to add one rule, for this program on private networks only; if you decline, Settings shows a notice with **Add the rule**. Choose a player in the output menu to play to it: the music is streamed at the source's format, the player's own remote and app work as usual, and its own volume stays as it is. Tracks of the same format follow without a gap where the player can; if not, Settings › Status says so. If another source takes the player over, CamelPlayer stops and says so; press play to take it back.

With a network player in use, Settings › Playback shows its own settings: the highest sample rate and bit depth to send, for a player that declares more than it plays (above them the conversion rule applies and the path shows as processed), whether DSD goes as DSF or becomes PCM, and **Gapless playback**, on by default. Turn that off only for a player that handles the next track badly: it then gets one track at a time, with a short pause at each change, and the signal path says so. Under Advanced, silence after a format change gives a player that mutes while it switches format up to 2 seconds of silence at the start of the stream, and the device is released 60 seconds into a pause, so a short pause resumes at once.

A network player's volume is its own. In Settings › DSP, **Volume control** offers **The player's volume**, the default where the player has one, or **Fixed**; CamelPlayer never lowers the level of the stream, so it stays bit-perfect. The volume button then opens a 0–100 slider and mute that show what the player reports, follow its remote and app, and change it only when you move them. CamelPlayer remembers only the choice, never a level. Some players report volume 0 and mute while they are in standby or set to fixed volume; the slider shows that as it is.

Apple's own AirPlay receivers appear under Network players too, marked (AirPlay): HomePods, Apple TVs and Macs. Other makers' receivers are played through UPnP instead. Turning one on asks it for a session, which carries no audio; one that refuses goes back off and stays that way until CamelPlayer restarts. A receiver CamelPlayer cannot use says why, and its switch stays off: a Mac, for one, refuses the connection CamelPlayer uses.

Choose a receiver in the output menu to play to it. A HomePod stereo pair is one receiver: each speaker gets the same stream and keeps time by itself, so packets lost on the way to one can put the two out of step until playback starts again. AirPlay 1 carries 44.1 kHz/16-bit stereo only: a source in that format arrives unchanged and the path says "Bit-perfect to the receiver", and any other goes through the conversion rule. A receiver plays about 2 seconds behind what is sent, so play, a seek and a pause take that long, and the position shown follows what you hear. Tracks of the same format follow without a gap. **The player's volume** sets the receiver's level with the slider, about -20 dB the first time; AirPlay cannot read a level back, so a change on the receiver itself is not shown. If another source takes the receiver or it stops answering, playback pauses and says so. AirPlay is a trademark of Apple Inc.

The backend is how audio reaches the device:

| Backend | Use it when | Notes |
| --- | --- | --- |
| WASAPI, exclusive (default) | Almost always | Bit-perfect. "Windows default device" moves playback to a new default device; a device you select pauses when it disappears. |
| WASAPI, shared | Other programs must play at the same time | Goes through the Windows mixer, so it is always converted and shown as Processed. |
| ASIO | Your DAC has its own ASIO driver | Bit-perfect and the only backend with native DSD. **Open control panel** shows the driver's own settings. |
| Kernel Streaming | A device without ASIO where WASAPI misbehaves | Talks to the driver directly. While it plays, Windows and other programs cannot use the device. |

The badges beside the volume button in the player bar show the path in use (in a narrow window only the lamp's dot stays): **Bit-perfect** means the file's samples reach the device unchanged, **Processed** that something converted them (shared mode, a fixed sample rate or a conversion you allowed). Click the lamp to open the signal path above the player bar. It starts with the verdict and the route from the file's format to the device's, then lists the steps in three stages: the source (the file and the decoder), the processing that acts, and the output (the step that fits the samples to the device, then the device). Each row shows the format after the step and a dot for its state: green bit-perfect, yellow lossless conversion, red processed, gray lossy source. A ring marks crossfade, which acts only when tracks change. When nothing processes the signal, the processing stage says so. Click anywhere else or press Esc to close the panel. The Status card in Settings lists the output format, the mode, the device buffer and underruns; "Requested ≠ active" means the device could not take a saved setting and what it uses instead.

When a device does not take a file as it is, CamelPlayer converts it by default: it resamples, downmixes to stereo, reduces the bit depth or decodes DTS, and the badge shows **Processed**. Set **When the device does not take a file as it is** to **Skip it and tell me** to play bit-perfect only; such files are then skipped. A rate the driver offers but cannot run, such as 64 kHz on Chord's ASIO driver, counts as one the device does not take. A WAV file that holds DTS surround instead of PCM, as DTS-CD rips do, is never played as PCM, which would be loud noise: it is decoded and downmixed when conversion is allowed, and skipped otherwise. **Sample rate** fixes one output rate for every PCM file (shown as Processed); leave it at **Follow the file** for bit-perfect playback.

## DSD playback

DSF and DSDIFF files are sent to the DAC as raw DSD in one of two ways, set per backend with **DSD output**:

- **Native DSD** (ASIO only, its default): the driver receives DSD directly.
- **DoP** (DSD over PCM, the default for WASAPI and Kernel Streaming): DSD packed into 24-bit PCM frames that a DoP-capable DAC unpacks. DoP needs a PCM rate of DSD rate / 16, so DSD256 needs 705.6 kHz and DSD512 needs 1411.2 kHz; few devices offer the latter.

While raw DSD plays, the **DSD** or **DoP** badge shows that volume and every other PCM processing step are bypassed.

**Convert to PCM** turns DSD into PCM at the **DSD to PCM sample rate** (88.2, 176.4 or 352.8 kHz) for DACs without DSD support. If you keep native or DoP output, **When the device takes neither native DSD nor DoP** decides what happens when the device refuses it: skip the file and tell you (default), or convert it to PCM. DSDIFF files compressed with DST are not supported.

## DSP: equalizer, crossfeed and convolution

Settings › DSP has three cards, **Volume and ReplayGain**, **Crossfade** and **DSP processing**; each long note shows its first sentence, and **Learn more** shows the rest. Everything there is off by default. DSP processing holds three kinds of processing: a parametric EQ, headphone crossfeed and convolution with an impulse response. They work in the order shown under **Order**, EQ → crossfeed → convolution by default, so a room or headphone correction sits nearest the speakers or headphones. Use the arrows to move one, and **Restore default order** to go back. The order changes the sound only where left and right are treated differently, such as EQ bands for one channel or a true-stereo response, next to crossfeed.

One preamp covers the whole chain: it is lowered automatically by the largest boost of everything that is on, so nothing clips, and the line at the top of the card shows by how much. Corrections that cancel each other cost no level.

**Volume.** **Volume control** is **Fixed** by default: CamelPlayer sends the music at full level and your DAC or amplifier sets the volume. The volume button in the player bar shows a lock then and says so when clicked. **Software** makes the button open a vertical volume slider (−80 to 0 dB) with the level above it and a mute button below; the mouse wheel over the button or the slider moves it 1 dB. At 0 dB playback stays bit-perfect. The Windows volume and media keys keep controlling the system volume.

**ReplayGain** plays music at an even loudness. **Track** levels each track, **Album** keeps the differences within an album, and **Auto** uses the album's value while the queue plays an album's tracks in order and the track's otherwise, including shuffle. **Preamp** adds to every value, **Without a value** applies to tracks that have none yet, and **Clipping prevention** lowers a gain that would push a track's peak over full scale. Values come from the files' ReplayGain tags; while ReplayGain is on, tracks without them are analyzed in the background (EBU R128), queued tracks first. **Throttled** analysis uses one thread and pauses while music plays from the same drive or share; **Fast** uses every core. The card shows how many tracks have a value. The results stay in the library and are never written to the files. A change applies from the next track.

**Crossfade** overlaps two tracks as one fades out and the next fades in, for the **Length** you set (1 to 10 seconds, 3 by default). It happens at every change of track, when the next one follows by itself and when you press Previous or Next or play another track or album; an album played in order stays gapless. Pause, stop, seeking and restarting a track act at once. The fade takes at most half of a short track. A track in another format is converted to the format already playing for the whole track, so nothing reopens mid-track; the device takes each file's own format again when playback next starts. Raw DSD never crossfades, and the signal path panel says when a track could not.

**Parametric EQ.** It starts with five flat bands: a low shelf at 100 Hz, peaking bands at 400 Hz, 1 kHz and 4 kHz, and a high shelf at 10 kHz.

- Drag a numbered point on the curve to change its band's frequency and gain, and turn the mouse wheel over it to change its Q. The rows under the curve take exact values, the filter type (peaking, low or high shelf, low or high pass, band pass, notch or all pass), the channels (both, left or right) and, for low and high pass, a slope of 6 to 48 dB per octave. **Add band** adds bands up to ten, and a band's check box turns it off without removing it.
- **Save as preset…** names the current bands, and the preset list switches between them. **Import…** reads an Equalizer APO configuration or an AutoEq ParametricEQ.txt file as a new preset named after the file, and **Export…** writes the bands in the Equalizer APO format. **Reset** goes back to the five flat bands; saved presets stay.

**Crossfeed** (for headphones) sends some of each channel to the other ear, as speakers do, using bs2b. **Default** is 700 Hz and 4.5 dB, **Chu Moy** 700 Hz and 6 dB, **Jan Meier** 650 Hz and 9.5 dB. **Custom** takes a cut-off of 300–2000 Hz and a feed of 1–15 dB; a lower feed sends more to the other ear.

**Convolution** applies an impulse response, such as a room correction filter. **Choose impulse response…** takes a .wav that is mono (used for both channels), stereo, or 4-channel true stereo (left to left, left to right, right to left, right to right), or a .zip holding one such .wav per sample rate. The rate that plays is taken from the .zip; when it is missing, the nearest rate is converted and cached. A response may have up to 1,048,576 samples per channel at the playing rate. The card shows the file's channels, length and rates.

The **DSP** button in the player bar turns each of the three and crossfade on or off, switches the ReplayGain mode and EQ presets and opens Settings › DSP. It is highlighted while the chain changes the sound.

Changes are heard within about a quarter of a second, without a gap. While the chain acts, the badge shows **Processed**, the signal path lists each step, and the output is dithered once to the device's bit depth. While nothing changes the signal, playback stays bit-perfect. After the last track, the end of a convolution's response plays out. Raw DSD (native or DoP) bypasses the chain; convert DSD to PCM to process it.

## Discord

Settings › Interface › **Show what's playing on Discord** puts the playing track on your Discord profile while the Discord desktop app runs: the title (also in your friends list), the artists, the album when you point at the picture, the progress bar, and a small dot for the signal path's state with its format, such as "FLAC 24-bit 96 kHz · Bit-perfect". Pausing or stopping clears it. The line under the switches says whether Discord was found; CamelPlayer looks again every 15 seconds, and the browser version of Discord cannot be reached. Discord shows it to others only while **Settings › Activity Privacy › Share your detected activities with others** is on there.

**Also show the cover** shows the file's own cover instead of the CamelPlayer icon. Discord fetches pictures itself from a public address, so CamelPlayer serves the cover, resized to 512 pixels without its metadata, through an anonymous Cloudflare quick tunnel that runs while music plays and closes 10 minutes after it stops. The first time, CamelPlayer downloads cloudflared (about 55 MB) from Cloudflare's GitHub releases into its data folder and checks it before running it. Without a cover, or until the tunnel is ready, the CamelPlayer icon stands in.

## Common problems

**A file is skipped with "The device cannot play this format bit-perfectly".** The device does not accept the file's sample rate, bit depth or channel count. Check that the right device is selected, try a different **Sample format**, or allow conversion (see above).

**No sound, or "Output device not connected".** Turn the DAC on and choose it in the output menu. The lists update by themselves when a device appears or goes away; **Refresh device lists** in Settings › Audio outputs reads them again at once. A device you selected yourself is never replaced by another one; choose another output or "Windows default device (WASAPI)".

**DSD plays as loud noise or hiss.** The DAC does not understand DoP (or the driver does not pass it unchanged). Set DSD output to **Convert to PCM**. Turn the volume down before trying DSD on a new device.

**Other programs are silent while CamelPlayer plays.** Exclusive mode, ASIO and Kernel Streaming take the device for themselves. Stop releases it at once; after a pause it is released after **Release the device after pausing** (one second by default). To play alongside other programs, use WASAPI shared mode.

**Clicks, missing beginnings or a short mute when the format changes.** Some DACs need time to switch between sample rates or between PCM and DSD. Raise **Silence after a format change** (Buffers and timing) to 200–500 ms.

**Dropouts or crackles.** The Status card counts underruns. Use a larger buffer: the ASIO buffer size (or the driver's control panel), a longer WASAPI device period or polling buffer, or a longer Kernel Streaming buffer. Keep the MMCSS task at Pro Audio.

**A library folder shows as offline.** The drive or NAS share cannot be reached. Its tracks stay in the library, marked unavailable, and come back when the folder does. New files are found automatically; if tags look out of date, **Rebuild the library** in Settings › Reset and data rereads every file and keeps statistics and playlists. **Remove folder** in Settings › Library first shows how many tracks, plays, favorites and playlist entries will go, then deletes them from the library; your music files are not touched. When a file disappears from a folder that can be reached, it stays in the library for 30 days, so a moved or renamed file keeps its plays and favorites, and is deleted after that.

**"CamelPlayer repaired damaged data".** A settings file or library database that could not be read is kept with the `.bad` extension in the data folder. The settings come back from the copy saved at the previous start. The library comes back from its daily backup, made while CamelPlayer is idle or as it exits, so changes since then may be missing. Without a usable copy the settings return to their defaults, or a new library starts and the music folders have to be added again.

**CamelPlayer says its data was written by a newer version.** An older CamelPlayer was started after a newer one had upgraded the settings or the library. Start the newer version again; the older one changed nothing.

**A hotkey does not work.** Settings › Hotkeys reports a combination another program already holds; press a different one.

**"Some program files are damaged or missing".** The first time each version starts, CamelPlayer compares its program folder with the release's list of files; **Check installation** in Settings › About does it at any time. **Details** lists the files. **Repair** downloads the same version from GitHub again and replaces the program files; your settings and library stay. If an antivirus program quarantined a file, restore it there or exclude the CamelPlayer folder first.

**Windows reports a missing DLL, or CamelPlayer does not start at all.** The zip was not extracted completely, CamelPlayer was started from inside the zip, or an antivirus program removed a file. Extract the whole zip again into a folder of its own and start `CamelPlayer.exe` from there.

## Reporting a problem

Open an issue on the project's GitHub page and fill in the form. In Settings › About, select **Create diagnostic bundle…** and attach the zip it saves. It holds the logs, settings, your output devices with the formats they accept, the result of the installation check, and the newest crash report. File and folder paths are left out unless you check **Include file and folder paths**. The crash report is a memory snapshot of CamelPlayer, so only attach the bundle where you are comfortable sharing it.
