// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "Server/Api/ServerApi.h"
#include "Engine/PlaybackEngine.h"

namespace camel::server {

class LocalServer final : public IServer {
public:
    // An existing settings directory also hosts library.db; an omitted or missing directory disables the library.
    explicit LocalServer(const Settings& settings, std::filesystem::path settingsFile = {},
                         audio::OutputFactory factory = audio::createOutput);
    ~LocalServer() override;
    CommandStamp submit(Command command) override;
    Snapshot snapshot() const override;
    void setListener(std::function<void(const Event&)> listener) override;
    void waitIdle() override;
    void shutdown() override;
    LibraryTrackPage browseTracks(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                  const CancellationToken& token = {}) const override;
    LibraryTrackPage searchTracks(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                  const CancellationToken& token = {}) const override;
    LibrarySearchResults searchLibrary(std::string query, int64_t offset, int64_t limit,
                                       std::optional<uint64_t> revision, const CancellationToken& token = {},
                                       bool totals = true) const override;
    PlayStatistics playStatistics(const PlayStatisticsQuery& query, const CancellationToken& token = {}) const override;
    void rereadLibrary() override;
    void clearArtworkCache() override;
    LibraryAlbumPage recentlyPlayedAlbums(int64_t offset, int64_t limit, const CancellationToken& token = {}) const override;
    LibraryAlbumPage browseAlbums(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, int64_t offset, int64_t limit,
                                  std::optional<uint64_t> revision, const CancellationToken& token = {}) const override;
    LibraryGroupPage browseGroups(LibraryGroupField field, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                  const CancellationToken& token = {}) const override;
    LibraryTrackRowPage browseTrackRows(TrackSort sort, bool descending, int64_t offset, int64_t limit,
                                        std::optional<uint64_t> revision, const CancellationToken& token = {}) const override;
    LibraryTrackRowPage smartTracks(SmartList list, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                    const CancellationToken& token = {}) const override;
    LibraryAlbumPage favoriteAlbums(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                    const CancellationToken& token = {}) const override;
    std::optional<ArtworkReference> artwork(const ArtworkRequest& request, const CancellationToken& token = {}) const override;
    LibraryAlbumDetail albumDetail(int64_t album, const CancellationToken& token = {}) const override;
    LibraryFolderListing browseFolder(std::optional<int64_t> folder, const CancellationToken& token = {}) const override;
    std::vector<LibraryRootInfo> libraryRoots() const override;
    LibraryScanProgress libraryScanProgress() const override;
    std::vector<PlaylistSummary> playlists() const override;
    PlaylistPage playlistItems(int64_t playlist, int64_t offset, int64_t limit) const override;
    SelectionPreview previewSelection(const std::vector<PlaylistSource>& sources,
                                      const CancellationToken& token = {}) const override;
    PlaylistChange createPlaylist(std::string name, const std::vector<PlaylistSource>& sources,
                                  const CancellationToken& token = {}) override;
    PlaylistChange addToPlaylist(int64_t playlist, std::optional<int64_t> revision, const std::vector<PlaylistSource>& sources,
                                 const CancellationToken& token = {}) override;
    PlaylistChange saveQueueAsPlaylist(std::string name) override;
    PlaylistChange editPlaylist(const PlaylistEdit& edit) override;
    bool undoPlaylistChange(const PlaylistUndo& undo) override;
    PlaylistChange importPlaylist(const std::filesystem::path& file) override;
    void exportPlaylist(int64_t playlist, const std::filesystem::path& file) const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
