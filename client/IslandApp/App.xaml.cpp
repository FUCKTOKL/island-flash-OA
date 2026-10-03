#include "pch.h"
#include "App.xaml.h"
#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

#include "IslandWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// ============ 唯一实例互斥 ============
// docs/02 §二：灵动岛不该开两个。但崩溃僵尸进程会攥着互斥锁误杀新实例，
// 所以冲突时用窗口存在性验证“真身”：无窗口 = 僵尸 → 放行
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"IF-OA.Island.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (FindWindowW(nullptr, L"IF-OA 灵动岛")) // 有活窗口才是真实例
        {
            CloseHandle(mutex);
            return 0;
        }
        // 无窗口：僵尸残留，抢占启动
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
        window = make<winrt::IslandApp::implementation::IslandWindow>(); // 注意：必须 implementation 命名空间（限定名会命中投影类→编译错）
        window.Activate();
    }
}
