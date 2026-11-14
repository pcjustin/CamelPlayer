// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <shobjidl.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace ui {

// The file chosen in the open dialog, or in the save dialog when `save`; nothing when it was cancelled. A save adds
// `extension` to a name typed without one and starts at `name`.
inline std::optional<std::filesystem::path> pickFile(HWND owner, std::span<const COMDLG_FILTERSPEC> filters, bool save = false,
                                                     const wchar_t* extension = nullptr, const std::wstring& name = {}) {
    const auto dialog = save ? winrt::create_instance<IFileSaveDialog>(CLSID_FileSaveDialog).as<IFileDialog>()
                             : winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog).as<IFileDialog>();
    dialog->SetFileTypes(UINT(filters.size()), filters.data());
    if (extension) dialog->SetDefaultExtension(extension);
    if (!name.empty()) dialog->SetFileName(name.c_str());
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(item.put())) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return std::nullopt;
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

} // namespace ui
