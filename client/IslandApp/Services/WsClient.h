#pragma once
// 极简 WS 客户端：MessageWebSocket（系统自带，契约 §三）
// 连接 /ws?token=<jwt>；30s 心跳 {"type":"ping"}；只收推送
// ponytail: 断线 5s 固定重连（指数退避 P1）；事件经 DispatcherQueue 编回 UI 线程
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <functional>

namespace ifoa
{
    class WsClient
    {
    public:
        using OnEvent = std::function<void(winrt::hstring type)>;

        void Start(winrt::hstring const& httpBase, winrt::hstring const& token,
                   winrt::Microsoft::UI::Dispatching::DispatcherQueue dq, OnEvent onEvent);
        void Stop() { m_stopped = true; if (m_socket) { try { m_socket.Close(); } catch (...) {} } }

    private:
        winrt::Windows::Foundation::IAsyncAction ConnectAsync();
        void SendPing();
        void ScheduleReconnect(); // 5s 后重连（UI 线程建定时器）

        winrt::Windows::Networking::Sockets::MessageWebSocket m_socket{ nullptr };
        winrt::Windows::Storage::Streams::DataWriter m_writer{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dq{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_ping{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_reconnect{ nullptr };
        winrt::hstring m_base;
        winrt::hstring m_token;
        winrt::hstring m_url;
        OnEvent m_onEvent;
        bool m_stopped{ true };
    };

    // ================= 实现（header-only） =================

    inline void WsClient::Start(winrt::hstring const& httpBase, winrt::hstring const& token,
                                winrt::Microsoft::UI::Dispatching::DispatcherQueue dq, OnEvent onEvent)
    {
        m_base = httpBase;
        m_token = token;
        m_dq = dq;
        m_onEvent = onEvent;
        m_stopped = false;

        std::wstring u(httpBase);
        if (u.rfind(L"http://", 0) == 0) u.replace(0, 7, L"ws://");
        m_url = winrt::hstring(u + L"/ws?token=" + std::wstring(token));

        ConnectAsync();
    }

    inline winrt::Windows::Foundation::IAsyncAction WsClient::ConnectAsync()
    {
        using namespace winrt;
        using namespace winrt::Windows::Networking::Sockets;
        try
        {
            m_socket = MessageWebSocket();
            m_socket.Control().MessageType(SocketMessageType::Utf8);

            // 收到推送：解析 type，编回 UI 线程再回调
            m_socket.MessageReceived([this](winrt::Windows::Networking::Sockets::MessageWebSocket const&,
                                            winrt::Windows::Networking::Sockets::MessageWebSocketMessageReceivedEventArgs const& e) {
                try
                {
                    auto dr = e.GetDataReader();
                    auto msg = dr.ReadString(dr.UnconsumedBufferLength());
                    hstring type;
                    try { type = Windows::Data::Json::JsonObject::Parse(msg).GetNamedString(L"type"); }
                    catch (...) { return; } // 非 JSON 忽略（服务端只发 JSON，防御）
                    auto dq = m_dq;
                    dq.TryEnqueue([this, type] { if (m_onEvent) m_onEvent(type); });
                }
                catch (...) {}
            });

            // 断开：非主动停止 → 5s 后重连
            m_socket.Closed([this](winrt::Windows::Networking::Sockets::IWebSocket const&,
                                   winrt::Windows::Networking::Sockets::WebSocketClosedEventArgs const&) {
                if (m_stopped) return;
                ScheduleReconnect();
            });

            co_await m_socket.ConnectAsync(winrt::Windows::Foundation::Uri(m_url));
            m_writer = winrt::Windows::Storage::Streams::DataWriter(m_socket.OutputStream());

            // 30s 心跳（契约：服务端 60s 无消息即断）
            m_ping = m_dq.CreateTimer();
            m_ping.Interval(std::chrono::seconds{ 30 });
            m_ping.Tick([this](auto&&, auto&&) { SendPing(); });
            m_ping.Start();
        }
        catch (...) { ScheduleReconnect(); }
    }

    inline void WsClient::ScheduleReconnect()
    {
        auto dq = m_dq;
        dq.TryEnqueue([this] {
            m_reconnect = m_dq.CreateTimer();
            m_reconnect.Interval(std::chrono::seconds{ 5 });
            m_reconnect.IsRepeating(false);
            m_reconnect.Tick([this](auto&&, auto&&) {
                if (!m_stopped) Start(m_base, m_token, m_dq, m_onEvent);
            });
            m_reconnect.Start();
        });
    }

    inline void WsClient::SendPing()
    {
        try
        {
            if (m_writer)
            {
                m_writer.WriteString(L"{\"type\":\"ping\"}");
                m_writer.StoreAsync(); // 失败由 Closed 事件兜底重连
            }
        }
        catch (...) {}
    }
}
