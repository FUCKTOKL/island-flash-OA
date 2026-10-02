#pragma once
// App：应用入口类，仅负责创建主窗口（壳逻辑都在 IslandWindow）
#include "App.g.h"

namespace winrt::IslandApp::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    private:
        Microsoft::UI::Xaml::Window window{ nullptr }; // 持有主窗口引用，防止提前析构
    };
}

namespace winrt::IslandApp::factory_implementation
{
    struct App : AppT<App, implementation::App>
    {
    };
}
