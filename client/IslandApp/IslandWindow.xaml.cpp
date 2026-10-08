#include "pch.h"
#include "IslandWindow.xaml.h"
#if __has_include("IslandWindow.g.cpp")
#include "IslandWindow.g.cpp"
#endif

// Win32 互操作：DWM 圆角 + 取 HWND
#include <dwmapi.h>
#include <microsoft.ui.xaml.window.h> // ::IWindowNative
#include <ShObjIdl_core.h>            // ::IInitializeWithWindow（桌面 FilePicker 必须挂 HWND）
#include <winrt/Windows.Storage.h>    // FileIO 落盘
#include <winrt/Windows.Storage.Pickers.h> // 文件选择器
#include <algorithm>
#include <cmath>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib") // 区域裁剪（CreateRoundRectRgn/SetWindowRgn 等）

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

    // 刘海形窗口区域（苹果13语言）：贴屏上缘直角 + 底部圆角。
    // WinUI3 无异形透明窗口，DWM 圆角又四角统一 → SetWindowRgn 硬裁剪；
    // XAML 侧 IslandFrame 同形 CornerRadius 叠渐变层，硬边视觉上不可见
    static void ApplyNotchRgn(HWND hwnd, int32_t w, int32_t h)
    {
        int r = 24; // 底部圆角半径（物理像素）；须大于 XAML 侧 20，深色层溢出裁剪边防漏白
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r, r);
        HRGN top = CreateRectRgn(0, 0, w + 1, r);
        CombineRgn(rgn, rgn, top, RGN_OR); // 顶部补成直角
        SetWindowRgn(hwnd, rgn, TRUE);
        DeleteObject(top); // rgn 所有权归窗口
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

        // 主题（注册表记忆，1 字节）：'1'=黑岛；令牌字典见 App.xaml
        std::vector<BYTE> dkb;
        m_dark = ifoa::RegReadBinary(L"dark", dkb) && !dkb.empty() && dkb[0] == '1';
        Root().RequestedTheme(m_dark ? ElementTheme::Dark : ElementTheme::Light);
        if (m_dark) ThemeIcon().Glyph(L"\uE708"); // 月；默认日(浅色)

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
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND; // 刘海形由 SetWindowRgn 接管，DWM 不圆角
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        // 去 DWM 默认白边框（DWMWA_BORDER_COLOR=34，DWMWA_COLOR_NONE）：白环主凶之一
        COLORREF noBorder = 0xFFFFFFFE;
        DwmSetWindowAttribute(m_hwnd, 34, &noBorder, sizeof(noBorder));

        // 整窗毛玻璃（Apple 风）；Win10/关闭透明效果时自动回退不透明
        // ponytail: 每个组件再用半透明 Border 叠出“岛”层次
        // SystemBackdrop 已移除：像素取证发现亚克力未被上层压暗（奶灰层=用户看到的"白边"）；
        // 刘海语言本就是纯黑实体，改用 IslandFrame 全不透明渐变直出

        // 初始位置：优先用持久化值，否则主屏顶部居中；始终钳制在工作区内
        auto wa = WorkArea();
        m_capRect = { wa.X + (wa.Width - CAP_W) / 2, wa.Y + TOP_MARGIN, CAP_W, CAP_H };
        LoadPosition();
        m_appWindow.MoveAndResize(m_capRect);
        ApplyNotchRgn(m_hwnd, m_capRect.Width, m_capRect.Height);
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
        auto r = LerpRect(m_from, m_to, e);
        m_appWindow.MoveAndResize(r);
        ApplyNotchRgn(m_hwnd, r.Width, r.Height); // 每帧同步裁剪，动画中形状不断裂

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

    void IslandWindow::OnThemeToggleClick(IInspectable const&, RoutedEventArgs const&)
    {
        m_dark = !m_dark;
        ApplyTheme();
    }

    void IslandWindow::ApplyTheme()
    {
        // 一行翻转主题：App.xaml 令牌字典 + 默认控件铬色全部自动跟随
        Root().RequestedTheme(m_dark ? ElementTheme::Dark : ElementTheme::Light);
        BYTE b = m_dark ? '1' : '0';
        ifoa::RegWriteBinary(L"dark", &b, 1); // 下次启动生效
        ThemeIcon().Glyph(m_dark ? L"\uE708" : L"\uE706"); // 月/日
        SelectTab(m_curTab); // 选中页签药丸颜色随主题刷新
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

        // 文件页：收藏集 + 群栏 + 两栏列表
        LoadFilesPage();
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

    // ============ 文件页（左右分栏，P0 平铺列表；docs/02 §四） ============

    static winrt::hstring FmtSize(double n)
    {
        wchar_t b[32]{};
        if (n >= 1048576.0) swprintf_s(b, L"%.1f MB", n / 1048576.0);
        else if (n >= 1024.0) swprintf_s(b, L"%.0f KB", n / 1024.0);
        else swprintf_s(b, L"%.0f B", n);
        return b;
    }

    static bool IsImageName(std::wstring const& name)
    {
        auto p = name.rfind(L'.');
        if (p == std::wstring::npos) return false;
        auto e = name.substr(p + 1);
        for (auto& c : e) c = towlower(c);
        return e == L"png" || e == L"jpg" || e == L"jpeg" || e == L"gif"
            || e == L"webp" || e == L"bmp";
    }

    // 文件行操作小按钮（透明底图标钮）
    static Controls::Button IconBtn(wchar_t const* glyph, wchar_t const* tip)
    {
        Controls::Button b;
        b.Background(Media::SolidColorBrush(Windows::UI::Color{ 0, 0, 0, 0 })); // ARGB 全 0 = 透明（Colors 类在此链路不可见，直接构造）
        b.BorderThickness(Thickness(0));
        b.Padding(Thickness(6, 3, 6, 3));
        b.CornerRadius(CornerRadius(6)); // 代码侧要传结构体，double 是 XAML 转换器专属
        Controls::ToolTipService::SetToolTip(b, box_value(tip));
        Controls::FontIcon fi;
        fi.Glyph(glyph);
        fi.FontSize(13);
        b.Content(fi);
        return b;
    }

    // 空态/失败提示行
    static void AddListHint(Controls::StackPanel const& panel, wchar_t const* text)
    {
        auto tb = Controls::TextBlock();
        tb.Text(text);
        tb.FontSize(12);
        tb.Opacity(0.5);
        panel.Children().Append(tb);
    }

    void IslandWindow::AddFileRow(Controls::StackPanel const& panel,
                                   winrt::Windows::Data::Json::JsonObject const& o, bool mine)
    {
        int id = static_cast<int>(o.GetNamedNumber(L"id"));
        auto name = o.GetNamedString(L"name");

        auto row = Controls::Grid();
        auto cd1 = Controls::ColumnDefinition();
        cd1.Width(GridLength(1, GridUnitType::Star));
        auto cd2 = Controls::ColumnDefinition();
        cd2.Width(GridLength(1, GridUnitType::Auto));
        row.ColumnDefinitions().Append(cd1);
        row.ColumnDefinitions().Append(cd2);

        auto info = Controls::StackPanel();
        info.Orientation(Controls::Orientation::Horizontal);
        info.Spacing(8);
        auto ic = Controls::FontIcon();
        ic.Glyph(IsImageName(std::wstring(name)) ? L"\uE8B9" : L"\uE7C3"); // 图片/文档图标
        ic.FontSize(14);
        ic.Opacity(0.7);
        auto texts = Controls::StackPanel();
        texts.Spacing(1);
        auto tb = Controls::TextBlock();
        tb.Text(name);
        tb.FontSize(13);
        tb.TextTrimming(TextTrimming::CharacterEllipsis);
        tb.MaxWidth(260);
        auto sz = Controls::TextBlock();
        sz.Text(FmtSize(o.GetNamedNumber(L"size")));
        sz.FontSize(11);
        sz.Opacity(0.5);
        texts.Children().Append(tb);
        texts.Children().Append(sz);
        info.Children().Append(ic);
        info.Children().Append(texts);
        Controls::Grid::SetColumn(info, 0);

        auto ops = Controls::StackPanel();
        ops.Orientation(Controls::Orientation::Horizontal);
        ops.Spacing(2);
        auto dl = IconBtn(L"\uE896", L"下载");
        dl.Click([this, id, name](IInspectable const&, RoutedEventArgs const&) { DownloadFile(id, name); });
        auto st = IconBtn(m_favIds.count(id) ? L"\uE735" : L"\uE734", L"收藏");
        st.Click([this, id](IInspectable const&, RoutedEventArgs const&) { ToggleFavorite(id); });
        ops.Children().Append(dl);
        ops.Children().Append(st);
        if (mine)
        {
            auto del = IconBtn(L"\uE74D", L"删除");
            del.Click([this, id](IInspectable const&, RoutedEventArgs const&) { DeleteMyFile(id); });
            ops.Children().Append(del);
        }
        Controls::Grid::SetColumn(ops, 1);

        row.Children().Append(info);
        row.Children().Append(ops);
        panel.Children().Append(row);
    }

    winrt::fire_and_forget IslandWindow::LoadMyFiles()
    {
        auto lifetime = get_strong();
        auto body = co_await m_api.GetAsync(L"/files?scope=personal");
        MyFileList().Children().Clear();
        if (body.empty()) { AddListHint(MyFileList(), L"（加载失败）"); co_return; }
        int n = 0;
        try
        {
            for (auto const& v : winrt::Windows::Data::Json::JsonArray::Parse(body))
            {
                AddFileRow(MyFileList(), v.GetObject(), true);
                if (++n >= 50) break; // ponytail: 平铺 P0 只列前 50；分页 P2
            }
        }
        catch (...) {}
        if (n == 0) AddListHint(MyFileList(), L"（空 · 右上「上传」）");
    }

    winrt::fire_and_forget IslandWindow::LoadPublicFiles()
    {
        auto lifetime = get_strong();
        std::wstring url = L"/files?scope=public";
        if (m_selectedGroup) url += L"&group_id=" + std::to_wstring(m_selectedGroup);
        auto body = co_await m_api.GetAsync(winrt::hstring(url));
        PublicFileList().Children().Clear();
        if (body.empty()) { AddListHint(PublicFileList(), L"（加载失败）"); co_return; }
        int n = 0;
        try
        {
            for (auto const& v : winrt::Windows::Data::Json::JsonArray::Parse(body))
            {
                AddFileRow(PublicFileList(), v.GetObject(), false);
                if (++n >= 50) break;
            }
        }
        catch (...) {}
        if (n == 0) AddListHint(PublicFileList(), L"（空 · 全员公共区仅管理员可传）");
    }

    winrt::fire_and_forget IslandWindow::LoadFilesPage()
    {
        auto lifetime = get_strong();
        // 已收藏集合（星标亮灭用）
        auto fb = co_await m_api.GetAsync(L"/favorites");
        m_favIds.clear();
        try
        {
            for (auto const& v : winrt::Windows::Data::Json::JsonArray::Parse(fb))
                m_favIds.insert(static_cast<int>(v.GetObject().GetNamedNumber(L"id")));
        }
        catch (...) {}

        // 群选择栏：全员公共区 + 我所在的群（群 = group 会话，docs/01）
        auto body = co_await m_api.GetAsync(L"/conversations");
        GroupCombo().Items().Clear();
        auto all = Controls::ComboBoxItem();
        all.Content(box_value(L"全员公共区"));
        all.Tag(box_value(0));
        GroupCombo().Items().Append(all);
        try
        {
            for (auto const& v : winrt::Windows::Data::Json::JsonArray::Parse(body))
            {
                auto o = v.GetObject();
                if (o.GetNamedString(L"type") != L"group") continue;
                auto it = Controls::ComboBoxItem();
                it.Content(box_value(o.GetNamedString(L"name")));
                it.Tag(box_value(static_cast<int>(o.GetNamedNumber(L"id"))));
                GroupCombo().Items().Append(it);
            }
        }
        catch (...) {}
        GroupCombo().SelectedIndex(0); // 触发 OnGroupChanged → LoadPublicFiles
        LoadMyFiles();
    }

    void IslandWindow::OnGroupChanged(IInspectable const&,
                                      Controls::SelectionChangedEventArgs const&)
    {
        auto it = GroupCombo().SelectedItem().try_as<Controls::ComboBoxItem>();
        if (!it || !it.Tag()) return; // Items 清空过程中会触发一次空选择
        m_selectedGroup = unbox_value<int>(it.Tag());
        LoadPublicFiles();
    }

    winrt::fire_and_forget IslandWindow::ToggleFavorite(int id)
    {
        auto lifetime = get_strong();
        std::wstring p = L"/files/" + std::to_wstring(id) + L"/favorite";
        if (m_favIds.count(id))
        {
            m_favIds.erase(id);
            co_await m_api.DeleteAsync(winrt::hstring(p));
        }
        else
        {
            m_favIds.insert(id);
            co_await m_api.PostAsync(winrt::hstring(p), L"{}");
        }
        LoadPublicFiles();
        LoadMyFiles();
    }

    winrt::fire_and_forget IslandWindow::DeleteMyFile(int id)
    {
        auto lifetime = get_strong();
        co_await m_api.DeleteAsync(winrt::hstring(L"/files/" + std::to_wstring(id)));
        LoadMyFiles();
    }

    void IslandWindow::OnUploadClick(IInspectable const&, RoutedEventArgs const&) { UploadFile(); }

    winrt::fire_and_forget IslandWindow::UploadFile()
    {
        auto lifetime = get_strong();
        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd); // 桌面必须挂 HWND，否则弹窗不出现
        picker.FileTypeFilter().Append(L"*"); // IVector 是 Append 不是 Add
        auto f = co_await picker.PickSingleFileAsync();
        if (!f) co_return;
        auto buf = co_await winrt::Windows::Storage::FileIO::ReadBufferAsync(f);
        co_await m_api.UploadPersonalAsync(L"/files/upload", f.Name(), buf);
        LoadMyFiles();
    }

    winrt::fire_and_forget IslandWindow::DownloadFile(int id, winrt::hstring name)
    {
        auto lifetime = get_strong();
        winrt::Windows::Storage::Pickers::FileSavePicker sp;
        sp.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        sp.SuggestedFileName(name);
        auto exts = winrt::single_threaded_vector<winrt::hstring>();
        std::wstring n(name);
        auto p = n.rfind(L'.');
        exts.Append(p == std::wstring::npos ? winrt::hstring(L".bin") : winrt::hstring(n.substr(p)));
        sp.FileTypeChoices().Insert(L"文件", exts);
        auto f = co_await sp.PickSaveFileAsync();
        if (!f) co_return;
        auto buf = co_await m_api.DownloadAsync(
            winrt::hstring(L"/files/" + std::to_wstring(id) + L"/download"));
        if (buf) co_await winrt::Windows::Storage::FileIO::WriteBufferAsync(f, buf);
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
        m_curTab = idx;
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

        // 选中页签药丸：随主题取色（黑岛青绿深/白岛薄荷薄染）
        auto sel = Media::SolidColorBrush(m_dark
            ? Windows::UI::Color{ 0xFF, 0x2A, 0x3D, 0x35 }
            : Windows::UI::Color{ 0xFF, 0xD9, 0xEE, 0xE3 });
        auto unsel = Media::SolidColorBrush(Windows::UI::Color{ 0, 0, 0, 0 });
        Tab0().Background(idx == 0 ? sel : unsel);
        Tab1().Background(idx == 1 ? sel : unsel);
        Tab2().Background(idx == 2 ? sel : unsel);
        Tab3().Background(idx == 3 ? sel : unsel);
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
            ApplyNotchRgn(m_hwnd, m_capRect.Width, m_capRect.Height);
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
