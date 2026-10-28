// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Updater/Install.h"

#include <windows.h>

#include <fstream>
#include <thread>
#include <vector>

#include "Updater/UpdateFiles.h"

namespace camel::updater {

namespace {

namespace fs = std::filesystem;
namespace files = camel::updatefiles;

std::vector<fs::path> children(const fs::path& folder) {
    std::vector<fs::path> names;
    for (const auto& entry : fs::directory_iterator(folder)) names.push_back(entry.path().filename());
    return names;
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
            for (const auto& name : children(o_.app)) {
                if (name == files::kBackup) continue;
                fs::rename(o_.app / name, backup_ / name);
                moved_.push_back(name);
            }
            copying_ = true;
            fs::copy(o_.files, o_.app, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
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
            for (const auto& entry : fs::directory_iterator(o_.files, error))
                if (entry.path().filename() != files::kBackup) fs::remove_all(o_.app / entry.path().filename(), error);
        for (const auto& name : moved_) {
            fs::rename(backup_ / name, o_.app / name, error);
            if (error) log(L"could not restore " + name.wstring());
        }
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
    std::vector<fs::path> moved_;
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
