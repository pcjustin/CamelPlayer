# CamelPlayer Server API, version 2

A CamelPlayer that plays music can be controlled by CamelPlayers on other computers. They talk through this API, which is open to any program on the LAN.

The traffic is plain WebSocket and HTTP, as in Roon. Anyone on the LAN can read it. No password crosses it.

## Finding a Server

A Server announces itself through mDNS/DNS-SD as `_camelplayer._tcp`.

- **Instance name**: the Server's name, which is its computer's name by default.
- **SRV record**: the host and port.
- **A records**: the host's IPv4 addresses.
- **TXT record**:
  - `id`: the Server's own ID. It stays the same when the address or port changes, so a Control point knows a Server by it.
  - `name`: its name.
  - `api`: the range of API versions it serves, such as `2-2`.

A Server listens on TCP port 9440. When another program holds that port, it uses the next free one, so read the port from the SRV record or from the Server's Settings. A Control point can also connect to an address typed by hand.

## Pairing

Every request carries the token of a paired device as `Authorization: Bearer <token>`: the WebSocket's opening request and every HTTP request, covers included. A request without a token the Server knows is answered with 401 and the error `notPaired`. Only finding the Server and asking to pair go without a token.

To pair, send `POST /pair` with the device's name, and with a code unless the Server has no owner yet. A CamelPlayer also sends its own Server ID as `server`, which the Server keeps with the device, and, when this Server paired with it before, the `proof` below:

```json
{"name": "Laptop", "code": "123456", "server": "…", "proof": "…"}
```

The answer is the device's token and ID:

```json
{"token": "…", "device": "…"}
```

The first device to ask claims a Server that has no owner, without a code. Every later device needs a code that a paired device was shown:

- **A code** has six digits. It is valid for five minutes and for one pairing.
- **After five wrong codes** in a row, pairing stops for five minutes, the right code included. The paired devices are sent the notice `pairingStopped`.
- **Names are unique**: no two paired devices share one, so a device whose name is taken is asked for another.
- **Two computers that paired trust each other both ways**: when computer A paired with Server B, B kept A's Server ID with the device, and A kept B's token. Once B asks to pair with A, it sends its own ID as `server` and, as `proof`, the SHA-256 of the token it gave A, in lowercase hex; A works it out from the token it keeps for B, and pairs B without a code. The trust ends when B revokes A or A forgets B's token.

Pairing errors:

| Status | Code | Meaning |
| --- | --- | --- |
| 403 | `codeNeeded` | The Server has an owner; ask a paired device for a code. |
| 403 | `wrongCode` | The code is wrong, used or expired. |
| 409 | `nameTaken` | A paired device has the name; `suggestion` is a free one, such as `Laptop (2)`. |
| 429 | `stopped` | Pairing has stopped after wrong codes, until `until` (milliseconds since 1970). |

A paired device may make these calls on the WebSocket:

| Call | Arguments | Result |
| --- | --- | --- |
| `pairingCode` | | `{"code": "…", "validMs": 300000}`; a new code replaces the last |
| `pairedDevices` | | each device's `id`, `name`, `paired`, whether it is `connected` now and the `zone` it follows |
| `revokeDevice` | device | `true` when it was paired; its connections close at once |
| `resetOwner` | | `null`; every device is forgotten and cut off, the caller included, and the next to ask claims the Server |
| `follow` | zone | `null`; the zone the caller follows, which `pairedDevices` shows |
| `renameDevice` | name | `{"name": …}`, or `{"error": "nameTaken", "suggestion": …}` when another paired device has it |
| `serverUpdate` | | `{"version", "release", "administrator", "playing", "connected"}`: the Server's CamelPlayer version; the newer release it finds on GitHub as `{"version", "notes"}`, or `null`; whether its program folder needs an administrator, in which case it is updated only at its own computer; the names of the zones playing and of the other devices connected, which an update stops and cuts off |
| `updateServer` | | `{"version": …}` once the Server has downloaded and checked the release; it then installs it with its own updater and restarts, its queues paused. `invalidArgument` when it is up to date, `notAllowed` when its program folder needs an administrator |

## The WebSocket

Open a WebSocket at `ws://<host>:<port>/api` (RFC 6455, version 13). Every message is one JSON text message.

Once the connection is open, the Server sends a `hello` message and then its first snapshot:

```json
{"hello": {"api": {"min": 2, "max": 2}, "server": {"id": "…", "name": "Study PC", "version": "0.6.0"}, "device": "…"}}
```

`api` is the range of API versions the Server serves: after a new version it keeps serving the one before for a while, so updating either computer first keeps the link. A Control point whose version is outside the range closes the connection and says which side needs an update: the Server when its own version is above `max`, itself when below `min`. `version` is the Server's CamelPlayer release.

`device` is the ID the Server gave the connecting device when it paired, as `pairedDevices` lists it.

### Calls

A call names one of the Server's operations. Its arguments go in an array, in the order listed below:

```json
{"id": 7, "call": "browseTracks", "args": [0, 50, null]}
```

`id` is any unsigned integer that is not already in use on the connection. The answer carries the same `id`, with either a result or an error:

```json
{"id": 7, "result": {"revision": 12, "total": 86541, "tracks": [ … ]}}
{"id": 7, "error": {"code": "revisionChanged", "message": "library query revision changed"}}
```

Calls run at the same time; a slow listing holds up no other call. A connection may have 32 calls under way at once. One more is answered with `busy`.

To cancel a call, send `{"cancel": 7}`. Its answer is then the error `cancelled`.

### Error codes

| Code | Meaning |
| --- | --- |
| `invalidArgument` | The call is unknown, or its arguments do not fit it. |
| `revisionChanged` | A listing was asked for at a library revision that has changed since. Ask again for the first page. |
| `notAllowed` | The Server's settings do not allow it, such as online artist pictures while they are off. |
| `cancelled` | The call was cancelled. |
| `busy` | Too many calls are under way on this connection. |
| `failed` | Anything else; `message` says what. |

A message that is not valid JSON, or that is neither a call nor a cancel, closes the connection with WebSocket status 1008. A frame that breaks RFC 6455 does too, and so does a message over 4 MB.

### Events

The Server pushes an event whenever its state changes:

```json
{"event": "stateChanged", "snapshot": { … }}
```

A notice is pushed the same way, as `{"notice": {"kind": "pairingStopped", "until": …}}`.

The event is `commandCompleted` when a command has finished. When several changes come while an event is still being sent, only the latest is sent, and it is marked `commandCompleted` if any of the ones it replaces was.

The snapshot holds every zone with its playback and its queue, the selected zone, the settings and the devices. It is the same JSON as the `snapshot` call answers, except for the queues. Its `revision` grows with every change and its `command` with every command, counted again from the start each time the Server starts, whose `run` then differs: a snapshot of another `run` is not older than one seen before.

Several devices may control the Server at once, and each may follow a zone of its own; the selected zone is the Server's own window's. To tell its users what another device did:

- each zone has `played`, the Server's count of starts when it last started playing, so the zone that started last has the most, and `change`, the last time a device paused it, stopped it or gave it another queue: `{"serial", "kind", "device", "name"}`, with `kind` one of `paused`, `stopped`, `replaced` and `transferred`, `device` the paired device's ID (empty for the Server's own window) and `name` its name;
- `transfer` is the last Transfer to…, `{"serial", "from", "to"}`, which the devices that followed `from` follow to `to`; a device whose zone was grouped into another follows the group, the zone whose `playback.receivers` lists its receiver;
- `notice` is the last change of no one zone, as `change` is with `kind` one of `deleted` (the output named in `zone`), `statisticsReset`, `libraryRebuilt` and `playbackReset`.

A command's sender is the paired device of the connection it comes on; a command's own `device` is not read. An `ApplySettings` with `"reset": true` is Settings › Reset playback and output. A `groupZones` command names a zone and, in `zones`, the zones that play with it from then on, which an empty list ungroups; zones group only with zones of their kind, AirPlay receivers with each other and this computer's outputs (WASAPI, ASIO and Kernel Streaming) with each other, one device never with itself, and a zone playing apart is refused, the snapshot's `commandError` saying `deviceInUse`. A group of this computer's outputs lists its members in its playback's `members`, `{"id", "name", "clock", "lost", "detail"}`, the first first: the one whose clock the others keep to, and why one stopped while the others play on; its clock and each member's delay are the settings' `groupClock` and `groupDelayMs` of its saved outputs, its members their `localGroup`.

**Queues** are sent as what changed since the last event on the same connection:

- **The first time**, a queue has all its `entries`.
- **When nothing changed**, the queue has `"same": true` and no `entries`.
- **Otherwise**, `"keep": [p, s]` keeps the first `p` and the last `s` entries sent before, and `entries` holds the entries between them.

So adding one entry to a queue of 25,000 sends one entry.

### The calls

Arguments are listed in order. `?` marks a value that may be `null`.

| Call | Arguments | Result |
| --- | --- | --- |
| `submit` | command | command stamp |
| `snapshot` | | snapshot, with whole queues |
| `waitIdle` | | the snapshot `revision` reached once every command sent before is done, which an event then shows |
| `browseTracks` | offset, limit, revision? | track page |
| `searchTracks` | query, offset, limit, revision? | track page |
| `searchLibrary` | query, offset, limit, revision?, totals | search results |
| `playStatistics` | query | play statistics |
| `rereadLibrary` | | `null` |
| `scanLibraryNow` | folder? | `started`, `running` or `offline` |
| `clearArtworkCache` | | `null` |
| `recentlyPlayedAlbums` | offset, limit | album page |
| `browseAlbums` | sort, filter?, offset, limit, revision? | album page |
| `browseGroups` | field, favoritesOnly, offset, limit, revision? | group page |
| `groupInitials` | field | initials |
| `albumInitials` | sort | initials |
| `creditParts` | credits | each credit's parts |
| `browseTrackRows` | sort, descending, filter?, offset, limit, revision? | track row page |
| `albumPosition` | sort, filter?, favorites, album | position? |
| `trackRowPosition` | sort, descending, filter?, file | position? |
| `artistPage` | filter | artist page |
| `smartTracks` | list, offset, limit, revision? | track row page |
| `favoriteAlbums` | offset, limit, revision? | album page |
| `favoriteArtists` | offset, limit, revision? | group page |
| `artistPicture` | name, lookUp | artist picture |
| `removeArtistPicture` | name | artist picture |
| `chooseArtistCandidate` | name, mbid | artist picture |
| `albumDetail` | album | album detail |
| `albumTrackPaths` | albums | paths |
| `browseFolder` | folder? | folder listing |
| `folderTracks` | folder | track rows |
| `folderTracksFrom` | track | track rows |
| `fileFolders` | file | folder IDs |
| `libraryRoots` | | library folders |
| `libraryRootContents` | root | counts |
| `libraryScanProgress` | | scan progress |
| `loudnessProgress` | | loudness progress |
| `playlists` | | playlist summaries |
| `playlistItems` | playlist, offset, limit | playlist page |
| `previewSelection` | sources | selection preview |
| `createPlaylist` | name, sources | playlist change |
| `addToPlaylist` | playlist, revision?, sources | playlist change |
| `saveQueueAsPlaylist` | name, zone | playlist change |
| `editPlaylist` | edit | playlist change |
| `undoPlaylistChange` | undo | `true` when undone |
| `dataRecovery` | | what opening the data repaired |
| `discordStatus` | | Discord status |
| `lyrics` | path | lyrics |
| `fileDetails` | path | file details |
| `waveform` | path | bytes as numbers, or `null` |
| `setDiscordTexts` | texts | `null` |
| `serverFolders` | path | the drives for `""`, a computer's disk shares for `\\computer`, else the folders in the path |
| `playerFirewallRule` | | `true` when Windows Firewall on the Server's computer has the rule network players need |

Enumerations travel as names in lower camel case, such as `"recentlyAdded"` and `"albumArtist"`. Objects travel as their fields. The field names are those of the structures in `src/Server/Api/ServerApi.h` and the files it includes.

## Plain HTTP

Files go over plain HTTP on the same port. Each request is answered on its own connection, which then closes.

| Request | Body | Answer |
| --- | --- | --- |
| `GET /artwork?kind=&id=&path=&name=&pixels=` | | The JPEG of an album, folder, track or artist (`kind`), with its cache key in `X-Artwork-Key`; 404 when there is none. |
| `POST /artist-picture?name=` | the image | the artist picture, as JSON |
| `POST /playlist?name=` | an M3U8 file | the playlist change of its import, as JSON |
| `GET /playlist?id=` | | the playlist as M3U8, with the Server's paths |
| `GET /diagnostic?paths=0\|1&version=` | | the diagnostic bundle, made on the Server, as a ZIP |
| `POST /upload?name=` | a file | `{"path": …}`: where the Server keeps it, in its data folder, for a setting such as the convolution's impulse response |
| `GET /upload?path=` | | a file the Server keeps from an upload, or the impulse response its settings name; 403 for any other |

An upload may be up to 64 MB. An error comes back as `{"error": {"code": …, "message": …}}` with status 400, 403, 413 or 500.
