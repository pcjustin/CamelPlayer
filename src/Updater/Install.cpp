// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Updater/Install.h"

#include <windows.h>
// windows.h comes first
#include <bcrypt.h>

#include <array>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "Updater/UpdateFiles.h"

#pragma comment(lib, "bcrypt.lib")

namespace camel::updater {

namespace {

namespace fs = std::filesystem;
namespace files = camel::updatefiles;

std::wstring fromUtf8(const std::string& text) {
    std::wstring wide(text.size(), L'\0');
    wide.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), int(wide.size()))));
    return wide;
}

// A path inside the program folder, or nothing.
std::optional<fs::path> inside(const fs::path& path) {
    auto normal = path.lexically_normal();
    if (normal.empty() || normal.has_root_name() || normal.has_root_directory()) return std::nullopt;
    const auto first = *normal.begin();
    if (first == L".." || first == L"." || first == files::kBackup) return std::nullopt;
    return normal;
}

std::optional<std::string> readAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct ManifestLine {
    fs::path path;
    std::string sha256;
};

// "<sha256>  <size>  <path>" lines; malformed ones and unsafe paths are left out.
std::vector<ManifestLine> manifestLines(const std::string& text) {
    std::vector<ManifestLine> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.ends_with('\r')) line.pop_back();
        const auto hashEnd = line.find("  ");
        const auto sizeEnd = hashEnd == std::string::npos ? hashEnd : line.find("  ", hashEnd + 2);
        if (sizeEnd == std::string::npos) continue;
        if (auto path = inside(fromUtf8(line.substr(sizeEnd + 2)))) lines.push_back({std::move(*path), line.substr(0, hashEnd)});
    }
    return lines;
}

// What the installed version's install-manifest.txt lists, and the list itself; nothing without one.
std::set<fs::path> listedFiles(const fs::path& app) {
    std::set<fs::path> listed;
    const auto text = readAll(app / files::kManifest);
    if (!text) return listed;
    listed.emplace(files::kManifest);
    for (auto& line : manifestLines(*text)) listed.insert(std::move(line.path));
    return listed;
}

std::string hex(const std::array<UCHAR, 32>& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    for (const UCHAR byte : digest) {
        text += kDigits[byte >> 4];
        text += kDigits[byte & 0xF];
    }
    return text;
}

std::string sha256Of(std::string bytes) {
    std::array<UCHAR, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()),
                                   ULONG(bytes.size()), digest.data(), ULONG(digest.size()))))
        throw std::runtime_error("SHA-256 failed");
    return hex(digest);
}

std::string sha256Of(const fs::path& file) {
    auto bytes = readAll(file);
    if (!bytes) throw std::runtime_error("a new file cannot be read");
    return sha256Of(std::move(*bytes));
}

std::vector<fs::path> filesIn(const fs::path& folder) {
    std::vector<fs::path> found;
    for (const auto& entry : fs::recursive_directory_iterator(folder))
        if (entry.is_regular_file()) found.push_back(entry.path().lexically_relative(folder));
    return found;
}

// The folders above `file` in the program folder that are left empty.
void removeEmptyFolders(const fs::path& app, const fs::path& file) {
    std::error_code error;
    for (auto folder = file.parent_path(); !folder.empty(); folder = folder.parent_path())
        if (!fs::remove(app / folder, error)) return;
}

std::wstring describe(const std::exception& e) {
    const std::string what = e.what();
    std::wstring wide(what.size(), L'\0');
    wide.resize(size_t(MultiByteToWideChar(CP_ACP, 0, what.data(), int(what.size()), wide.data(), int(wide.size()))));
    return wide;
}

class Installation {
public:
    Installation(const Options& options, const Host& host) : o_(options), host_(host), backup_(o_.app / files::kBackup) {}

    Outcome run() {
        log(L"installing " + o_.version + L" into " + o_.app.wstring());
        try {
            snapshotData();
            fs::remove_all(backup_);
            fs::create_directory(backup_);
            // Only the package's files move: the release zip has no top folder, so other files may lie beside them.
            incoming_ = filesIn(o_.files);
            if (!o_.manifestSha256.empty()) checkList();
            auto owned = listedFiles(o_.app);
            owned.insert(incoming_.begin(), incoming_.end());
            for (const auto& name : owned) {
                if (!fs::is_regular_file(o_.app / name)) continue;
                fs::create_directories((backup_ / name).parent_path());
                fs::rename(o_.app / name, backup_ / name);
                moved_.push_back(name);
            }
            copying_ = true;
            for (const auto& name : incoming_) {
                fs::create_directories((o_.app / name).parent_path());
                fs::copy_file(o_.files / name, o_.app / name, fs::copy_options::overwrite_existing);
                // Checked once copied, where only the updater can change it.
                if (!expected_.empty() && sha256Of(o_.app / name) != expected_.at(name))
                    throw std::runtime_error("a new file changed after the player checked it");
            }
            for (const auto& name : moved_) removeEmptyFolders(o_.app, name);
        } catch (const std::exception& e) {
            log(L"installing failed: " + describe(e));
            return rollBack();
        }
        std::error_code error;
        fs::remove(o_.data / files::kStarted, error);
        writeResult(o_.data, L"installed", o_.version, o_.previous);
        host_.startPlayer(o_.app / files::kPlayer);
        if (!started()) {
            log(L"the new version did not start");
            if (host_.playerRunning()) host_.endPlayer();
            return rollBack();
        }
        fs::remove_all(backup_, error);
        fs::remove_all(o_.dataBackup, error);
        fs::remove(o_.data / files::kStarted, error);
        fs::remove(o_.data / files::kResult, error);
        log(L"installed " + o_.version);
        return Outcome::Installed;
    }

private:
    void log(const std::wstring& text) const {
        if (host_.log) host_.log(text);
    }

    // The new files must be the ones the player checked, as an elevated updater installs from a folder any program of
    // the user can write: their list as it was, nothing more and nothing less than it names.
    void checkList() {
        const auto text = readAll(o_.files / files::kManifest);
        if (!text || sha256Of(*text) != o_.manifestSha256)
            throw std::runtime_error("install-manifest.txt is not the one the player checked");
        for (auto& line : manifestLines(*text)) expected_[std::move(line.path)] = std::move(line.sha256);
        expected_[files::kManifest] = o_.manifestSha256;
        for (const auto& name : incoming_)
            if (!expected_.contains(name)) throw std::runtime_error("a new file is not in install-manifest.txt");
        if (incoming_.size() != expected_.size()) throw std::runtime_error("a file in install-manifest.txt is missing");
    }

    void snapshotData() {
        fs::remove_all(o_.dataBackup);
        fs::create_directories(o_.dataBackup);
        for (const auto* name : files::kDataFiles)
            if (fs::exists(o_.data / name))
                fs::copy_file(o_.data / name, o_.dataBackup / name, fs::copy_options::overwrite_existing);
        snapshotTaken_ = true;
    }

    bool started() const {
        using namespace std::chrono;
        const auto start = steady_clock::now();
        for (;;) {
            if (fs::exists(o_.data / files::kStarted)) return true;
            const auto elapsed = steady_clock::now() - start;
            if (elapsed >= host_.startTimeout) return false;
            if (elapsed >= host_.launchGrace && !host_.playerRunning()) return false;
            std::this_thread::sleep_for(milliseconds(100));
        }
    }

    // Puts the previous version's files and data back and starts it; it reports the undone update.
    Outcome rollBack() {
        std::error_code error;
        if (copying_)
            for (const auto& name : incoming_) {
                fs::remove(o_.app / name, error);
                removeEmptyFolders(o_.app, name);
            }
        for (const auto& name : moved_) {
            fs::create_directories((o_.app / name).parent_path(), error);
            fs::rename(backup_ / name, o_.app / name, error);
            if (error) log(L"could not restore " + name.wstring());
        }
        for (const auto& name : moved_) removeEmptyFolders(backup_, name);
        if (fs::is_empty(backup_, error)) fs::remove(backup_, error);
        if (snapshotTaken_)
            for (const auto* name : files::kDataFiles) {
                if (fs::exists(o_.dataBackup / name, error))
                    fs::copy_file(o_.dataBackup / name, o_.data / name, fs::copy_options::overwrite_existing, error);
                else
                    fs::remove(o_.data / name, error);
            }
        fs::remove_all(o_.dataBackup, error);
        fs::remove(o_.data / files::kStarted, error);
        writeResult(o_.data, L"failed", o_.version);
        log(L"rolled back to the previous version");
        host_.startPlayer(o_.app / files::kPlayer);
        return Outcome::RolledBack;
    }

    const Options& o_;
    const Host& host_;
    const fs::path backup_;
    std::vector<fs::path> incoming_;           // the new version's files
    std::map<fs::path, std::string> expected_; // their SHA-256, when checked
    std::vector<fs::path> moved_;              // into the backup
    bool copying_ = false;
    bool snapshotTaken_ = false;
};

} // namespace

Outcome install(const Options& options, const Host& host) {
    return Installation(options, host).run();
}

void writeResult(const std::filesystem::path& data, const wchar_t* kind, const std::wstring& version,
                 const std::wstring& previous) {
    std::wofstream out(data / files::kResult);
    out << kind << L' ' << version;
    if (!previous.empty()) out << L' ' << previous;
    out << L'\n';
}

} // namespace camel::updater
