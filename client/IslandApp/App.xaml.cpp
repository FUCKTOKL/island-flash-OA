#include "pch.h"
#include "App.xaml.h"
#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

#include "IslandWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// ============ 唯一实例互斥 ============
// 灵动岛常驻顶部，开两个会重叠错乱；已在跑则直接退出（doc/02 §二 实现要点）
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"IF-OA.Island.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex);
        return 0; // 已有实例，安静退出
    }
    // WinUI3 解包应用标准入口：Application::Start 回调里创建 App
    Application::Start([](auto&&) { winrt::make<winrt::IslandApp::implementation::App>(); });
    if (mutex)
    {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}

namespace winrt::IslandApp::implementation
{
    App::App()
    {
        InitializeComponent();
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        window = make<IslandApp::IslandWindow>();
        window.Activate();
    }
}
