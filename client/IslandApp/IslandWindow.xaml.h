#pragma once
// 岛壳窗口：两态切换/拖动/动画/时钟（对应 docs/02 §二"窗口模型与状态机"）
#include "IslandWindow.g.h"

namespace winrt::IslandApp::implementation
{
    struct IslandWindow : IslandWindowT<IslandWindow>
    {
        IslandWindow();

        // ---- XAML 事件（必须 public，生成代码引用） ----
        void OnTabClick(Windows::Foundation::IInspectable const& sender,
                        Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnCollapseClick(Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRootPointerPressed(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnRootPointerMoved(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnRootPointerReleased(Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);

    private:
        // ---- 壳生命周期 ----
        void SetupShell();          // HWND/AppWindow/无边框/置顶/圆角/Mica/初始位置
        void LoadPosition();        // 位置持久化读取（LocalSettings）
        void SavePosition() const;  // 位置持久化写入

        // ---- 两态 ----
        void Expand();              // 胶囊 → 展开（13 帧缓动逐帧 Resize + 内容 spring）
        void Collapse();            // 展开 → 胶囊
        void PlayEnterAnimation();  // 内容 scale 0.92→1 + 淡入（Composition）
        void SelectTab(int idx);    // 页签切换（骨架：占位页 Visibility 切换）

        void UpdateClock();
        void OnClockTick(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&);
        void OnAnimTick(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&);
        void OnActivated(winrt::Microsoft::UI::Xaml::Window const&,
                         Microsoft::UI::Xaml::WindowActivatedEventArgs const& args);

        // ---- 状态 ----
        HWND m_hwnd{};                                        // 本窗口句柄
        winrt::Microsoft::UI::Windowing::AppWindow m_appWindow{ nullptr };
        winrt::Windows::Graphics::RectInt32 m_capRect{};      // 胶囊矩形（物理像素）
        winrt::Windows::Graphics::RectInt32 m_expRect{};      // 展开矩形
        winrt::Windows::Graphics::RectInt32 m_from{};         // 动画起点
        winrt::Windows::Graphics::RectInt32 m_to{};           // 动画终点
        bool m_expanded{ false };    // 当前是否展开
        bool m_animating{ false };   // 动画进行中（期间禁止再切态）
        bool m_dragging{ false };    // 胶囊拖动中（区别于点击）
        winrt::Windows::Foundation::Point m_dragStart{ -1, -1 }; // 按下点（窗口内坐标）
        int32_t m_dragOriginX{ 0 }, m_dragOriginY{ 0 };       // 按下时窗口位置
        int m_frame{ 0 };            // 动画帧计数
        winrt::Windows::System::DispatcherQueueTimer m_clock{ nullptr };     // 秒时钟
        winrt::Windows::System::DispatcherQueueTimer m_animTimer{ nullptr }; // 60fps 动画
    };
}

namespace winrt::IslandApp::factory_implementation
{
    struct IslandWindow : IslandWindowT<IslandWindow, implementation::IslandWindow>
    {
    };
}
