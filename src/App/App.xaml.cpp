#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "util/Log.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::CamelPlayer::implementation {

App::App() {
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        camel::log::error("unhandled XAML exception: %s", winrt::to_string(e.Message()).c_str());
#if defined _DEBUG
        if (IsDebuggerPresent()) __debugbreak();
#endif
    });
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    window_ = make<MainWindow>();
    window_.Activate();
}

}  // namespace winrt::CamelPlayer::implementation
