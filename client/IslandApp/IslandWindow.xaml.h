#pragma once
// 岛壳窗口：两态切换/拖动/动画/时钟（对应 docs/02 §二"窗口模型与状态机"）
#include "IslandWindow.g.h"
#include "Services/ApiClient.h"
#include "Services/WsClient.h"
#include <set>

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
        void OnLoginClick(Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAddTodoClick(Windows::Foundation::IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnExitClick(Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnThemeToggleClick(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnUploadClick(Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnGroupChanged(Windows::Foundation::IInspectable const&,
                            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

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
        void EnterLoginState();     // 无 token：强制展开仅登录卡（docs/02 状态机）
        winrt::fire_and_forget DoLogin();
        winrt::fire_and_forget AfterLogin(winrt::hstring token); // 拉个人信息 + WS 连接
        void OnWsEvent(winrt::hstring const& type);              // WS 推送分发（UI 线程）
        void UpdateBadge();                                       // 胶囊未读角标
        void ApplyTheme();                                        // 黑白主题切换（含注册表记忆）
        winrt::fire_and_forget LoadTodos();                       // 工作台：待办岛
        winrt::fire_and_forget LoadWeather();                     // 工作台：天气岛（Open-Meteo 直连）
        winrt::fire_and_forget ToggleTodo(int id);                // 勾选完成 → PUT done
        winrt::fire_and_forget AddTodo();                          // 快速添加（协程，XAML 事件只做 void 转发）
        void AddTodoRow(int id, winrt::hstring const& content);   // 待办行（无绑定，直建控件）

        // ---- 文件页（左右分栏，P0 平铺列表） ----
        winrt::fire_and_forget LoadFilesPage();   // 登录后一次：收藏集 + 群栏 + 两栏
        winrt::fire_and_forget LoadMyFiles();
        winrt::fire_and_forget LoadPublicFiles();
        winrt::fire_and_forget UploadFile();      // FileOpenPicker → multipart POST
        winrt::fire_and_forget DownloadFile(int id, winrt::hstring name); // GET 下载流 + 另存
        winrt::fire_and_forget ToggleFavorite(int id);
        winrt::fire_and_forget DeleteMyFile(int id);
        void AddFileRow(Microsoft::UI::Xaml::Controls::StackPanel const& panel,
                        Windows::Data::Json::JsonObject const& o, bool mine); // 文件行

        void OnRootLoaded(Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::RoutedEventArgs const&); // Root 加载完再初始化壳
        void OnAnimTick(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&);
        void OnActivated(Windows::Foundation::IInspectable const&,
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
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_animTimer{ nullptr }; // 60fps 动画
        bool m_loginMode{ false }; // 登录态（无 token 时展开仅登录卡）
        ifoa::ApiClient m_api;     // 后端 API 客户端（Windows.Web.Http）
        ifoa::WsClient m_ws;       // 后端推送（MessageWebSocket）
        int m_unread{ 0 };         // 合并未读（消息+通知，docs/02 §二）
        std::set<int> m_favIds;    // 已收藏文件 id（星标亮灭）
        int m_selectedGroup{ 0 };  // 文件页群栏：0=全员公共区，>0=群 id
        bool m_dark{ false };      // 主题：false=白岛，true=黑岛（注册表 dark 字节记忆）
        int m_curTab{ 0 };         // 当前页签（切主题时刷新选中药丸用）
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dq{ nullptr }; // UI 调度器（WS 事件编回）
    };
}

namespace winrt::IslandApp::factory_implementation
{
    struct IslandWindow : IslandWindowT<IslandWindow, implementation::IslandWindow>
    {
    };
}
