#pragma once
// App：应用入口类，仅负责创建主窗口（壳逻辑都在 IslandWindow）
// 注意：App.idl 无构造声明（Application 派生类惯例），
// 故 cppwinrt 不生成工厂 AppT，这里也不写 factory_implementation 段
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
