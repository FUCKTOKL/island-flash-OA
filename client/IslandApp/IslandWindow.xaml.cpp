#include "pch.h"
#include "IslandWindow.xaml.h"
#if __has_include("IslandWindow.g.cpp")
#include "IslandWindow.g.cpp"
#endif

// Win32 互操作：DWM 圆角 + 取 HWND
#include <dwmapi.h>
#include <microsoft.ui.xaml.window.h> // ::IWindowNative
#include <algorithm>
#include <cmath>
#pragma comment(lib, "dwmapi.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace mw = winrt::Microsoft::UI::Windowing;      // AppWindow / OverlappedPresenter / DisplayArea
using winrt::Windows::Graphics::RectInt32;

namespace winrt::IslandApp::implementation
{
    // ---- 布局常量（docs/02 §二，物理像素）----
    constexpr int32_t CAP_W = 216, CAP_H = 36;   // 胶囊态
    constexpr int32_t EXP_W = 1280, EXP_H = 480; // 展开态
    constexpr int32_t TOP_MARGIN = 0;            // 岛贴屏幕上边缘（拍板：不留空隙）
    constexpr int32_t ANIM_FRAMES = 13;          // 220ms @ 60fps（docs/02 实现要点）
    constexpr int32_t DRAG_THRESHOLD = 6;        // 位移超过此值 = 拖动，否则 = 点击

    // 骨架用主屏工作区；多屏 GetFromPoint 升级路径见 docs/02 §二
    static RectInt32 WorkArea() { return mw::DisplayArea::Primary().WorkArea(); }

    // 线性插值两矩形：X 与 W 同步插值 → 中心点恒定（"以胶囊 x 为轴向两侧展开"）
    static RectInt32 LerpRect(RectInt32 const& a, RectInt32 const& b, double t)
    {
        auto L = [t](int32_t u, int32_t v) { return static_cast<int32_t>(std::lround(u + (v - u) * t)); };
        return { L(a.X, b.X), L(a.Y, b.Y), L(a.Width, b.Width), L(a.Height, b.Height) };
    }

    // 诊断日志：追加到 %TEMP%\ifoa-island-crash.log（定位启动期异常用）
    static void LogDiag(char const* msg)
    {
        char path[MAX_PATH]{};
        if (GetTempPathA(MAX_PATH, path))
        {
            strcat_s(path, "ifoa-island-crash.log");
            HANDLE f = CreateFileA(path, FILE_APPEND_DATA, 0, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD w{}; WriteFile(f, msg, (DWORD)strlen(msg), &w, nullptr); CloseHandle(f);
            }
        }
    }

    IslandWindow::IslandWindow()
    {
        InitializeComponent();
        m_dq = this->DispatcherQueue(); // WS 事件编回 UI 线程用

        // Esc 收起（Accelerator 挂根元素，无需焦点管理；docs/02 状态机）
        Input::KeyboardAccelerator esc;
        esc.Key(Windows::System::VirtualKey::Escape);
        esc.Invoked([this](auto&&, auto&&) { Collapse(); });
        Root().KeyboardAccelerators().Append(esc);

        // 失活收起：点击窗外 = Deactivated（docs/02 状态机）
        Activated({ this, &IslandWindow::OnActivated });

        // 60fps 动画定时器（两态切换逐帧 Resize）；从 Window 自身取调度器
        // （GetForCurrentThread 在解包启动链路上会返回空 → 空指针崩溃）
        auto dq = this->DispatcherQueue();
        m_animTimer = dq.CreateTimer();
        m_animTimer.Interval(std::chrono::milliseconds{ 16 });
        m_animTimer.Tick({ this, &IslandWindow::OnAnimTick });

        /* SetupShell 延迟到 Root Loaded：构造期可组合基类 inner 尚未挂接，
        try_as<IWindowNative> 会返回空导致空指针崩溃（0xC0000005） */
        Root().Loaded({ this, &IslandWindow::OnRootLoaded });

        SelectTab(0);
    }

    void IslandWindow::OnRootLoaded(IInspectable const&, RoutedEventArgs const&)
    {
        try
        {
            SetupShell();
        }
        catch (winrt::hresult_error const& e)
        {
            // XAML 回调里未处理异常会 fail-fast(0xC0000409)，先拦下记录现场
            char buf[256]{};
            sprintf_s(buf, "SetupShell hr=0x%08lX msg=%ws\n",
                      static_cast<unsigned long>(e.code().value), e.message().c_str());
            LogDiag(buf);
        }
        catch (...)
        {
            LogDiag("SetupShell unknown-exception\n");
        }

        // 无 token → 强制展开仅登录卡（docs/02 状态机：首启/登出后）
        m_api.SetBase(ifoa::ApiClient::LoadServer());
        LoginServer().Text(ifoa::ApiClient::LoadServer());
        if (ifoa::ApiClient::LoadToken().empty())
            EnterLoginState();
        else // 已有 token：恢复会话（拉个人信息 + WS）
        {
            auto token = ifoa::ApiClient::LoadToken();
            m_api.SetToken(token);
            AfterLogin(token);
        }
    }

    // ================= 壳初始化 =================
    void IslandWindow::SetupShell()
    {
        // 取 HWND（WinUI3 窗口互操作标准姿势）
        this->try_as<::IWindowNative>()->get_WindowHandle(&m_hwnd);

        m_appWindow = mw::AppWindow::GetFromWindowId(
            winrt::Microsoft::UI::WindowId{ reinterpret_cast<uint64_t>(m_hwnd) });
        m_appWindow.Title(L"IF-OA 灵动岛"); // 单实例验证用（FindWindow 按标题找）

        auto pres = m_appWindow.Presenter().as<mw::OverlappedPresenter>();
        pres.SetBorderAndTitleBar(false, false); // 无边框无标题栏（胶囊形态的前提）
        pres.IsAlwaysOnTop(true);                // 置顶；不抢独占全屏 z-order（docs/02 接受被盖）
        pres.IsResizable(false);                 // 尺寸只由两态动画控制
        m_appWindow.IsShownInSwitchers(false);   // 不进 Alt-Tab

        // WS_EX_TOOLWINDOW：不出现在任务栏
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE,
            GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);

        // DWM 圆角（Win10 自动忽略）。
        // ponytail: WASDK 无真透明窗口，18px 全胶囊圆角做不了；DWM ROUND(≈8px) 先近似，
        // 升级路径 = WASDK 透明窗口 API 成熟后改自绘圆角
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

        // 整窗毛玻璃（Apple 风）；Win10/关闭透明效果时自动回退不透明
        // ponytail: 每个组件再用半透明 Border 叠出“岛”层次
        SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{});

        // 初始位置：优先用持久化值，否则主屏顶部居中；始终钳制在工作区内
        auto wa = WorkArea();
        m_capRect = { wa.X + (wa.Width - CAP_W) / 2, wa.Y + TOP_MARGIN, CAP_W, CAP_H };
        LoadPosition();
        m_appWindow.MoveAndResize(m_capRect);
    }

    // 位置持久化：Win32 注册表（HKCU\Software\IF-OA\Island）。
    // ponytail: docs/02 写的是 LocalSettings，但 WASDK 1.5 无解包版
    // Microsoft.Windows.Storage.ApplicationData；升级路径 = WASDK ≥1.6 后换回
    void IslandWindow::LoadPosition()
    {
        DWORD x = 0, size = sizeof(x), type = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\IF-OA\\Island", L"island_x",
                         RRF_RT_REG_DWORD, &type, &x, &size) == ERROR_SUCCESS)
        {
            auto wa = WorkArea();
            m_capRect.X = std::clamp(static_cast<int32_t>(x), wa.X, wa.X + wa.Width - CAP_W);
        }
    }

    void IslandWindow::SavePosition() const
    {
        HKEY k{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\IF-OA\\Island", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            DWORD x = static_cast<DWORD>(m_capRect.X);
            RegSetValueExW(k, L"island_x", 0, REG_DWORD,
                           reinterpret_cast<BYTE const*>(&x), sizeof(x));
            RegCloseKey(k);
        }
    }

    // ================= 两态切换 =================
    void IslandWindow::Expand()
    {
        if (m_expanded || m_animating) return;
        m_expanded = true;
        m_animating = true;

        // 展开矩形：以胶囊中心为轴向两侧展开、向下生长，钳制屏幕边界（docs/02 §二）
        auto wa = WorkArea();
        int32_t cx = m_capRect.X + m_capRect.Width / 2;
        m_expRect = { cx - EXP_W / 2, m_capRect.Y, EXP_W, EXP_H };
        m_expRect.X = std::clamp(m_expRect.X, wa.X, wa.X + wa.Width - EXP_W);
        m_expRect.Y = std::clamp(m_expRect.Y, wa.Y, wa.Y + wa.Height - EXP_H);

        // 内容立即切换（登录态显示登录卡，否则展开面板）；进入动画在首帧布局完成后触发
        CapsulePanel().Visibility(Visibility::Collapsed);
        if (m_loginMode)
        {
            LoginPanel().Visibility(Visibility::Visible);
        }
        else
        {
            ExpandedPanel().Visibility(Visibility::Visible);
            ExpandedPanel().IsHitTestVisible(true);
        }

        m_from = m_capRect;
        m_to = m_expRect;
        m_frame = 0;
        m_animTimer.Start();
    }

    void IslandWindow::Collapse()
    {
        if (!m_expanded || m_animating) return;
        m_expanded = false;
        m_animating = true;

        ExpandedPanel().IsHitTestVisible(false);
        // ponytail: 收起无退出动画，直接反向插值；需要时同 PlayEnterAnimation 对称加一份
        m_from = m_expRect;
        m_to = m_capRect;
        m_frame = 0;
        m_animTimer.Start();
    }

    void IslandWindow::OnAnimTick(IInspectable const&, IInspectable const&)
    {
        ++m_frame;

        // 首帧：布局已完成，此刻启动内容 spring 动画（保证 ActualSize/CenterPoint 正确）
        if (m_frame == 1 && m_expanded)
            PlayEnterAnimation();

        double t = (std::min)(1.0, m_frame / static_cast<double>(ANIM_FRAMES)); // (std::min)防 windows.h 宏
        double e = 1.0 - std::pow(1.0 - t, 3.0); // ease-out cubic
        m_appWindow.MoveAndResize(LerpRect(m_from, m_to, e));

        if (m_frame >= ANIM_FRAMES)
        {
            m_animTimer.Stop();
            m_animating = false;
            if (!m_expanded) // 收起完成：换回胶囊内容
            {
                ExpandedPanel().Visibility(Visibility::Collapsed);
                LoginPanel().Visibility(Visibility::Collapsed);
                CapsulePanel().Visibility(Visibility::Visible);
            }
        }
    }

    // 内容区 scale 0.92→1 + 淡入（Composition，docs/02 §二"实现要点"）
    void IslandWindow::PlayEnterAnimation()
    {
        auto visual = Hosting::ElementCompositionPreview::GetElementVisual(ContentHost());
        auto comp = visual.Compositor();

        auto size = ContentHost().ActualSize(); // {宽,高}（DIP，float2 字段为小写 x/y）
        visual.CenterPoint({ size.x / 2.0f, size.y / 2.0f, 0.0f });

        auto easing = comp.CreateCubicBezierEasingFunction({ 0.55f, 0.0f }, { 0.15f, 1.0f });
        auto const dur = std::chrono::milliseconds{ 220 };

        auto op = comp.CreateScalarKeyFrameAnimation();
        op.InsertKeyFrame(0.0f, 0.0f);
        op.InsertKeyFrame(1.0f, 1.0f, easing);
        op.Duration(dur);
        visual.StartAnimation(L"Opacity", op);

        auto sc = comp.CreateVector3KeyFrameAnimation();
        sc.InsertKeyFrame(0.0f, { 0.92f, 0.92f, 1.0f });
        sc.InsertKeyFrame(1.0f, { 1.0f, 1.0f, 1.0f }, easing);
        sc.Duration(dur);
        visual.StartAnimation(L"Scale", sc);
    }

    // ================= 页签 =================
    void IslandWindow::OnTabClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        int idx = _wtoi(sender.as<FrameworkElement>().Tag().as<hstring>().c_str()); // FrameworkElement 在 Xaml 命名空间
        SelectTab(idx);
    }

    void IslandWindow::OnExitClick(IInspectable const&, RoutedEventArgs const&)
    {
        // 退出：单窗口应用，关窗即进程结束（消息循环自然退）
        this->Close();
    }

    void IslandWindow::OnCollapseClick(IInspectable const&, RoutedEventArgs const&)
    {
        Collapse();
    }

    // ============ 登录 ============
    void IslandWindow::EnterLoginState()
    {
        m_loginMode = true;
        Expand();
    }

    void IslandWindow::OnLoginClick(IInspectable const&, RoutedEventArgs const&)
    {
        DoLogin();
    }

    // 登录后置：拉个人信息 + WS 连接（未读角标）
    winrt::fire_and_forget IslandWindow::AfterLogin(winrt::hstring token)
    {
        auto lifetime = get_strong();
        auto me = co_await m_api.GetAsync(L"/api/auth/me");
        if (!me.empty())
        {
            try
            {
                auto obj = winrt::Windows::Data::Json::JsonObject::Parse(me);
                MeWelcome().Text(L"欢迎，" + obj.GetNamedString(L"name") + L"（" + obj.GetNamedString(L"role") + L"）");
            }
            catch (...) { /* me 拉取失败不阻断主流程 */ }
        }
        m_ws.Start(m_api.Base(), token, m_dq,
                   [this](winrt::hstring type) { OnWsEvent(type); });

        // 工作台小组件：待办 + 天气
        LoadTodos();
        LoadWeather();
    }

    void IslandWindow::OnWsEvent(winrt::hstring const& type)
    {
        // docs/02 §二：胶囊角标 = 消息 + 通知合并计数
        if (type == L"message.new" || type == L"notification.new")
        {
            ++m_unread;
            UpdateBadge();
        }
    }

    void IslandWindow::UpdateBadge()
    {
        BadgeText().Text(winrt::hstring(std::to_wstring(m_unread)));
        Badge().Visibility(m_unread > 0 ? Visibility::Visible : Visibility::Collapsed);
    }

    // ============ 工作台小组件（v1 内联；后续组件化进 Widgets/） ============

    // WMO 天气码 → 中文（覆盖常用段，懒人映射）
    static wchar_t const* WmoText(int code)
    {
        if (code == 0) return L"晴";
        if (code <= 2) return L"多云";
        if (code == 3) return L"阴";
        if (code <= 48) return L"雾";
        if (code < 60) return L"毛毛雨";
        if (code < 70) return L"雨";
        if (code < 80) return L"雪";
        if (code < 90) return L"阵雨";
        return L"雷暴";
    }

    winrt::fire_and_forget IslandWindow::LoadWeather()
    {
        auto lifetime = get_strong();
        // ponytail: 城市写死北京；设置中心做好后从注册表读 lat/lon
        auto body = co_await m_api.GetUrlAsync(
            L"https://api.open-meteo.com/v1/forecast?latitude=39.9042&longitude=116.4074"
            L"&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=auto&forecast_days=2");
        if (body.empty())
        {
            WeatherNow().Text(L"—");
            WeatherDetail().Text(L"获取失败");
            co_return;
        }
        try
        {
            auto d = winrt::Windows::Data::Json::JsonObject::Parse(body).GetNamedObject(L"daily");
            auto num = [](winrt::Windows::Data::Json::IJsonValue const& v)
                { return static_cast<int>(std::lround(v.GetNumber())); };
            auto const& codes = d.GetNamedArray(L"weather_code");
            auto const& maxs = d.GetNamedArray(L"temperature_2m_max");
            auto const& mins = d.GetNamedArray(L"temperature_2m_min");

            std::wstring now = std::to_wstring(num(maxs.GetAt(0))) + L"° · " + WmoText(num(codes.GetAt(0)));
            std::wstring det = L"明天 " + std::to_wstring(num(maxs.GetAt(1))) + L"° / "
                + std::to_wstring(num(mins.GetAt(1))) + L"° · " + WmoText(num(codes.GetAt(1)));
            WeatherNow().Text(winrt::hstring(now));
            WeatherDetail().Text(winrt::hstring(det));
        }
        catch (...) { WeatherDetail().Text(L"解析失败"); }
    }

    void IslandWindow::AddTodoRow(int id, winrt::hstring const& content)
    {
        auto row = Controls::StackPanel();
        row.Orientation(Controls::Orientation::Horizontal);
        row.Spacing(8);

        auto tb = Controls::TextBlock();
        tb.Text(content);
        tb.FontSize(13);
        tb.TextTrimming(TextTrimming::CharacterEllipsis); // 在 Xaml 命名空间，非 Controls
        tb.MaxWidth(320);

        auto chk = Controls::CheckBox();
        chk.Checked([this, id](IInspectable const&, RoutedEventArgs const&) { ToggleTodo(id); });

        row.Children().Append(chk);
        row.Children().Append(tb);
        TodoList().Children().Append(row);
    }

    winrt::fire_and_forget IslandWindow::LoadTodos()
    {
        auto lifetime = get_strong();
        auto body = co_await m_api.GetAsync(L"/todos");
        TodoList().Children().Clear();
        if (body.empty())
        {
            auto tb = Controls::TextBlock();
            tb.Text(L"（加载失败）");
            tb.FontSize(12);
            tb.Opacity(0.5);
            TodoList().Children().Append(tb);
            co_return;
        }
        try
        {
            int shown = 0;
            for (auto const& v : winrt::Windows::Data::Json::JsonArray::Parse(body))
            {
                auto o = v.GetObject();
                if (o.GetNamedBoolean(L"done")) continue; // docs/02：只看未完成前 5 条
                if (shown++ >= 5) break;
                AddTodoRow(static_cast<int>(o.GetNamedNumber(L"id")), o.GetNamedString(L"content"));
            }
            if (shown == 0)
            {
                auto tb = Controls::TextBlock();
                tb.Text(L"无待办，添加一条？");
                tb.FontSize(12);
                tb.Opacity(0.5);
                TodoList().Children().Append(tb);
            }
        }
        catch (...) {}
    }

    winrt::fire_and_forget IslandWindow::ToggleTodo(int id)
    {
        auto lifetime = get_strong();
        std::wstring p = L"/todos/" + std::to_wstring(id);
        co_await m_api.PutAsync(winrt::hstring(p), L"{\"done\":true}");
        LoadTodos();
    }

    void IslandWindow::OnAddTodoClick(IInspectable const&, RoutedEventArgs const&)
    {
        AddTodo();
    }

    winrt::fire_and_forget IslandWindow::AddTodo()
    {
        auto lifetime = get_strong();
        auto content = TodoInput().Text();
        if (content.empty()) co_return;
        winrt::Windows::Data::Json::JsonObject o;
        o.Insert(L"content", winrt::Windows::Data::Json::JsonValue::CreateStringValue(content));
        co_await m_api.PostAsync(L"/todos", o.Stringify());
        TodoInput().Text(L"");
        LoadTodos();
    }

    winrt::fire_and_forget IslandWindow::DoLogin()
    {
        auto lifetime = get_strong(); // 协程期间窗口保活
        LoginBtn().IsEnabled(false);
        LoginBtn().Content(box_value(L"连接中…"));
        LoginError().Visibility(Visibility::Collapsed);

        m_api.SetBase(LoginServer().Text());
        auto token = co_await m_api.LoginAsync(LoginUser().Text(), LoginPass().Password());

        LoginBtn().IsEnabled(true);
        LoginBtn().Content(box_value(L"登录"));
        if (!token.empty())
        {
            ifoa::ApiClient::SaveToken(token);             // DPAPI 加密存注册表
            ifoa::ApiClient::SaveServer(LoginServer().Text());
            m_loginMode = false;
            LoginPanel().Visibility(Visibility::Collapsed);
            ExpandedPanel().Visibility(Visibility::Visible);
            ExpandedPanel().IsHitTestVisible(true);
            SelectTab(0);
            m_api.SetToken(token);
            AfterLogin(token);
        }
        else
        {
            LoginError().Text(m_api.LastError());
            LoginError().Visibility(Visibility::Visible);
        }
    }

    // 骨架版页签：占位页 Visibility 切换 + 字重高亮；
    // P1 升级路径 = Controls/SegmentedTabs（选中指示条滑动 + 内容横移淡入，docs/02 §三）
    void IslandWindow::SelectTab(int idx)
    {
        try
        {
        Page0().Visibility(idx == 0 ? Visibility::Visible : Visibility::Collapsed);
        Page1().Visibility(idx == 1 ? Visibility::Visible : Visibility::Collapsed);
        Page2().Visibility(idx == 2 ? Visibility::Visible : Visibility::Collapsed);
        Page3().Visibility(idx == 3 ? Visibility::Visible : Visibility::Collapsed);

        Tab0().FontWeight(idx == 0 ? winrt::Microsoft::UI::Text::FontWeights::Bold() : winrt::Microsoft::UI::Text::FontWeights::Normal());
        Tab1().FontWeight(idx == 1 ? winrt::Microsoft::UI::Text::FontWeights::Bold() : winrt::Microsoft::UI::Text::FontWeights::Normal());
        Tab2().FontWeight(idx == 2 ? winrt::Microsoft::UI::Text::FontWeights::Bold() : winrt::Microsoft::UI::Text::FontWeights::Normal());
        Tab3().FontWeight(idx == 3 ? winrt::Microsoft::UI::Text::FontWeights::Bold() : winrt::Microsoft::UI::Text::FontWeights::Normal());
        }
        catch (winrt::hresult_error const& e)
        {
            char buf[192]{};
            sprintf_s(buf, "SelectTab hr=0x%08lX %ws\n",
                      static_cast<unsigned long>(e.code().value), e.message().c_str());
            LogDiag(buf);
        }
    }

    // ================= 胶囊拖动 / 点击 =================
    // 规则（docs/02 §二）：按下后位移 ≤ 阈值且松开 → 点击展开；超出 → 拖动换位，松开持久化
    void IslandWindow::OnRootPointerPressed(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (m_expanded) return; // 展开态整窗交互，不拖动
        auto pt = e.GetCurrentPoint(Root());
        if (!pt.Properties().IsLeftButtonPressed()) return;

        m_dragStart = pt.Position();
        m_dragOriginX = m_capRect.X;
        m_dragOriginY = m_capRect.Y;
        m_dragging = false;
        Root().CapturePointer(e.Pointer());
    }

    void IslandWindow::OnRootPointerMoved(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (m_expanded || m_dragStart.X < 0) return;
        auto p = e.GetCurrentPoint(Root()).Position();
        double dx = p.X - m_dragStart.X;
        double dy = p.Y - m_dragStart.Y;

        if (!m_dragging && (std::fabs(dx) > DRAG_THRESHOLD || std::fabs(dy) > DRAG_THRESHOLD))
            m_dragging = true;

        if (m_dragging)
        {
            auto wa = WorkArea();
            m_capRect.X = m_dragOriginX + static_cast<int32_t>(dx);
            m_capRect.Y = m_dragOriginY + static_cast<int32_t>(dy);
            m_capRect.X = std::clamp(m_capRect.X, wa.X, wa.X + wa.Width - CAP_W);
            m_capRect.Y = std::clamp(m_capRect.Y, wa.Y, wa.Y + wa.Height - CAP_H);
            m_appWindow.MoveAndResize(m_capRect);
        }
    }

    void IslandWindow::OnRootPointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        if (m_expanded || m_dragStart.X < 0) return;
        Root().ReleasePointerCapture(e.Pointer());

        bool wasClick = !m_dragging;
        m_dragStart = { -1, -1 };
        m_dragging = false;

        if (wasClick) Expand();      // 点击胶囊 → 展开
        else SavePosition();         // 拖动结束 → 持久化位置
    }

    // ================= 失活收起 =================
    void IslandWindow::OnActivated(IInspectable const&, WindowActivatedEventArgs const& args)
    {
        if (args.WindowActivationState() == WindowActivationState::Deactivated)
        {
            Collapse(); // 点击窗外 → 收起；胶囊态时 Collapse 自带守卫，无副作用
        }
    }
}
