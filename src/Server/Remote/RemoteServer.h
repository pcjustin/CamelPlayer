// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "Server/Api/ServerApi.h"
#include "Server/Remote/DevicePairing.h"

namespace camel::server {

// The Server refused a connection because this device is not paired with it, or no longer is.
class NotPaired : public std::runtime_error {
public:
    NotPaired() : std::runtime_error("Server API: this device is not paired with the Server") {}
};

// The Server refused to pair: "codeNeeded", "wrongCode", "stopped" (until `until`) or "nameTaken" (`suggestion` is free).
class PairingRefused : public std::runtime_error {
public:
    PairingRefused(std::string why, std::string free, int64_t stoppedUntil)
        : std::runtime_error("Server API: pairing refused: " + why), code(std::move(why)), suggestion(std::move(free)),
          until(stoppedUntil) {}
    std::string code, suggestion;
    int64_t until = 0;
};

// Another computer's CamelPlayer through the Server API (docs/Plan.md §17.8, docs/Server-API.md), the same IServer
// the window uses for its own: calls go out as JSON and wait for their answers, each on the caller's thread; the
// Server's events come in pushed and keep the snapshot; covers, uploads and downloads go over plain HTTP.
class RemoteServer final : public IServer, public DevicePairing {
public:
    struct Options {
        std::string host;
        uint16_t port = 9440;
        std::filesystem::path cache;             // where covers fetched are kept; a temporary folder when empty
        std::chrono::milliseconds timeout{5000}; // to connect and to get the first snapshot
        std::string token;                       // this device's, from pair()
        // A Control point's own settings.json: what belongs to its computer (copyComputerSettings) is read from it and
        // saved to it, never to the Server, whose settings show for the rest. Empty shows the Server's alone.
        std::filesystem::path ownSettings;
    };
    // Connects and waits for the Server's first snapshot; throws NotPaired without a token the Server knows, and
    // std::runtime_error when it cannot connect.
    explicit RemoteServer(Options options);
    ~RemoteServer() override;
    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    bool connected() const;
    nlohmann::json hello() const; // what the Server said of itself as the connection opened
    std::string device() const;   // this device's ID, as the Server knows it

    struct Paired {
        std::string token, device;
    };
    // Asks the Server to pair this device as `name`, with a code a paired device shows unless it is ownerless; throws
    // PairingRefused, or std::runtime_error when it cannot reach it.
    static Paired pair(const std::string& host, uint16_t port, const std::string& name, const std::string& code,
                       std::chrono::milliseconds timeout = std::chrono::seconds(5));
    // Pairing as any paired device may do it (docs/Plan.md 17.8).
    std::string pairingCode() override;
    nlohmann::json pairedDevices() override;
    bool revokeDevice(const std::string& device) override;
    void resetOwner() override; // cuts this device off too
    // Notices the Server sends, such as pairing having stopped after wrong codes; on the connection's thread.
    void setNoticeListener(std::function<void(const nlohmann::json&)> listener);

    CommandStamp submit(Command command) override;
    Snapshot snapshot() const override;
    void setListener(std::function<void(const Event&)> listener) override;
    void waitIdle() override;
    void shutdown() override; // closes the connection; the Server goes on
    LibraryTrackPage browseTracks(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                  const CancellationToken& token = {}) const override;
    LibraryTrackPage searchTracks(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                  const CancellationToken& token = {}) const override;
    LibrarySearchResults searchLibrary(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                       const CancellationToken& token = {}, bool totals = true) const override;
    PlayStatistics playStatistics(const PlayStatisticsQuery& query, const CancellationToken& token = {}) const override;
    void rereadLibrary() override;
    LibraryScanStart scanLibraryNow(std::optional<int64_t> folder) override;
    void clearArtworkCache() override;
    LibraryAlbumPage recentlyPlayedAlbums(int64_t offset, int64_t limit, const CancellationToken& token = {}) const override;
    LibraryAlbumPage browseAlbums(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, int64_t offset, int64_t limit,
                                  std::optional<uint64_t> revision, const CancellationToken& token = {}) const override;
    LibraryGroupPage browseGroups(LibraryGroupField field, bool favoritesOnly, int64_t offset, int64_t limit,
                                  std::optional<uint64_t> revision, const CancellationToken& token = {}) const override;
    std::vector<LibraryInitial> groupInitials(LibraryGroupField field, const CancellationToken& token = {}) const override;
    std::vector<LibraryInitial> albumInitials(AlbumSort sort, const CancellationToken& token = {}) const override;
    std::vector<std::vector<LibraryNamePart>> creditParts(const std::vector<std::string>& credits,
                                                          const CancellationToken& token = {}) const override;
    LibraryTrackRowPage browseTrackRows(TrackSort sort, bool descending, const std::optional<LibraryGroupFilter>& filter,
                                        int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                        const CancellationToken& token = {}) const override;
    std::optional<int64_t> albumPosition(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, bool favorites,
                                         int64_t album, const CancellationToken& token = {}) const override;
    std::optional<int64_t> trackRowPosition(TrackSort sort, bool descending, const std::optional<LibraryGroupFilter>& filter,
                                            int64_t file, const CancellationToken& token = {}) const override;
    LibraryArtistPage artistPage(const LibraryGroupFilter& artist, const CancellationToken& token = {}) const override;
    LibraryTrackRowPage smartTracks(SmartList list, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                    const CancellationToken& token = {}) const override;
    LibraryAlbumPage favoriteAlbums(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                    const CancellationToken& token = {}) const override;
    LibraryGroupPage favoriteArtists(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                     const CancellationToken& token = {}) const override;
    std::optional<ArtworkReference> artwork(const ArtworkRequest& request, const CancellationToken& token = {}) const override;
    ArtistPicture artistPicture(const std::string& name, bool lookUp, const CancellationToken& token = {}) override;
    ArtistPicture setArtistPicture(const std::string& name, const std::vector<uint8_t>& image) override;
    ArtistPicture removeArtistPicture(const std::string& name) override;
    ArtistPicture chooseArtistCandidate(const std::string& name, const std::string& mbid,
                                        const CancellationToken& token = {}) override;
    LibraryAlbumDetail albumDetail(int64_t album, const CancellationToken& token = {}) const override;
    std::vector<std::string> albumTrackPaths(const std::vector<int64_t>& albums,
                                             const CancellationToken& token = {}) const override;
    LibraryFolderListing browseFolder(std::optional<int64_t> folder, const CancellationToken& token = {}) const override;
    std::vector<LibraryTrackRow> folderTracks(int64_t folder, const CancellationToken& token = {}) const override;
    std::vector<LibraryTrackRow> folderTracksFrom(int64_t track, const CancellationToken& token = {}) const override;
    std::vector<int64_t> fileFolders(int64_t file) const override;
    std::vector<LibraryRootInfo> libraryRoots() const override;
    LibraryRootContents libraryRootContents(int64_t root) const override;
    LibraryScanProgress libraryScanProgress() const override;
    LoudnessProgress loudnessProgress() const override;
    std::vector<PlaylistSummary> playlists() const override;
    PlaylistPage playlistItems(int64_t playlist, int64_t offset, int64_t limit) const override;
    SelectionPreview previewSelection(const std::vector<PlaylistSource>& sources,
                                      const CancellationToken& token = {}) const override;
    PlaylistChange createPlaylist(std::string name, const std::vector<PlaylistSource>& sources,
                                  const CancellationToken& token = {}) override;
    PlaylistChange addToPlaylist(int64_t playlist, std::optional<int64_t> revision, const std::vector<PlaylistSource>& sources,
                                 const CancellationToken& token = {}) override;
    PlaylistChange saveQueueAsPlaylist(std::string name, const std::string& zone = {}) override;
    PlaylistChange editPlaylist(const PlaylistEdit& edit) override;
    bool undoPlaylistChange(const PlaylistUndo& undo) override;
    PlaylistChange importPlaylist(const std::filesystem::path& file) override;
    void exportPlaylist(int64_t playlist, const std::filesystem::path& file) const override;
    void writeDiagnosticBundle(const std::filesystem::path& file, bool includePaths,
                               const std::string& appVersion) const override;
    DataRecovery dataRecovery() const override;
    DiscordStatus discordStatus() const override;
    Lyrics lyrics(const std::string& pathUtf8) const override;
    FileDetails fileDetails(const std::string& pathUtf8) const override;
    std::optional<std::vector<uint8_t>> waveform(const std::string& pathUtf8) const override;
    void setDiscordTexts(const DiscordTexts& texts) override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace camel::server
