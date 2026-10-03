#include "pch.h"
#include "App.xaml.h"
#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

#include "IslandWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// ============ 崩溃取证 ============
// 未处理异常时把异常码+地址写入 %TEMP%\ifoa-island-crash.log，
// 配合链接器 map 文件（IslandApp.map）可直接定位到函数
static LONG WINAPI CrashLogger(EXCEPTION_POINTERS* ep)
{
    char buf[128]{};
    sprintf_s(buf, "code=0x%08lX addr=%p\n",
              ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    char path[MAX_PATH]{};
    if (GetTempPathA(MAX_PATH, path))
    {
        strcat_s(path, "ifoa-island-crash.log");
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA, 0, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD w{}; WriteFile(f, buf, (DWORD)strlen(buf), &w, nullptr); CloseHandle(f);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ============ 唯一实例互斥 ============
// docs/02 §二：灵动岛不该开两个。但崩溃僵尸进程会攥着互斥锁误杀新实例，
// 所以冲突时用窗口存在性验证“真身”：无窗口 = 僵尸 → 放行
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetUnhandledExceptionFilter(CrashLogger); // 崩溃取证先行
    winrt::init_apartment(winrt::apartment_type::single_threaded); // 必需！否则线程无 DispatcherQueue → 构造窗口时空指针崩溃
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
