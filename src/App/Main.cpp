// Entry point: logging, crash dumps and the UI language override must be in place before XAML starts.
#include "pch.h"

#include "App.xaml.h"
#include "server/ServerApi.h"
#include "util/CrashDump.h"
#include "util/Log.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto logs = camel::dataDirectory() / L"logs";
    camel::installCrashDumps(logs);
    camel::log::init(logs, L"CamelPlayer");
    camel::log::info("CamelPlayer starting");

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    auto server = camel::server::createLocalServer();
    try {
        const auto s = server->snapshot().settings;
        if (!s.language.empty()) // "" = follow Windows (docs/Plan.md §8.5)
            winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(
                winrt::to_hstring(s.language));
    } catch (const winrt::hresult_error& e) {
        camel::log::warn("language override failed: 0x%08X", static_cast<unsigned>(e.code()));
    }
    ::winrt::Microsoft::UI::Xaml::Application::Start(
        [server](auto&&) { ::winrt::make<::winrt::CamelPlayer::implementation::App>(server); });
    server->waitIdle();
    return 0;
}
