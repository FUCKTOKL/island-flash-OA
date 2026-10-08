#pragma once
// 极简 API 客户端：Windows.Web.Http 封装（系统自带，不引第三方库；AGENTS 约束）
// ponytail: 仅登录 + token/服务器地址持久化；其余接口按页面需要再加
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h> // DefaultRequestHeaders 在此
#include <winrt/Windows.Storage.Streams.h> // UnicodeEncoding 在这里（Http 枚举借用）
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <vector>
#pragma comment(lib, "crypt32.lib")

namespace ifoa
{
    class ApiClient
    {
    public:
        // 服务器基地址（含 http://host:port）
        void SetBase(winrt::hstring const& base) { m_base = base; }
        winrt::hstring Base() const { return m_base; }
        void SetToken(winrt::hstring const& token); // 后续请求带 Authorization
        winrt::hstring LastError() const { return m_lastError; }

        // POST /api/auth/login：成功返回 token，失败返回空串（原因在 LastError）
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
            LoginAsync(winrt::hstring const& user, winrt::hstring const& pass);

        // 通用 GET（需先 SetToken）：200 返回 body，否则空串 + LastError
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
            GetAsync(winrt::hstring const& path);

        // POST/PUT JSON：200 返回 body，否则空串 + LastError
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
            PostAsync(winrt::hstring const& path, winrt::hstring const& json);
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
            PutAsync(winrt::hstring const& path, winrt::hstring const& json);

        // 直连外部 URL（天气 Open-Meteo，不走后端）：返回 body 或空串
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
            GetUrlAsync(winrt::hstring const& url);

        // DELETE：同上套路
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
            DeleteAsync(winrt::hstring const& path);

        // 下载：返回字节缓冲（调用方 FileIO::WriteBufferAsync 落盘）；失败返回 null
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::Streams::IBuffer>
            DownloadAsync(winrt::hstring const& path);

        // multipart 上传（v1 固定 scope=personal；公共区上传入口 P2 再加）
        winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
            UploadPersonalAsync(winrt::hstring const& path, winrt::hstring const& fileName,
                                winrt::Windows::Storage::Streams::IBuffer const& data);

        // ---- 持久化（注册表 HKCU\Software\IF-OA\Island）----
        // token 用 DPAPI 加密：PasswordVault 需要包标识，解包应用用不了
        static void SaveToken(winrt::hstring const& token);
        static winrt::hstring LoadToken();
        static void SaveServer(winrt::hstring const& url);
        static winrt::hstring LoadServer();

    private:
        winrt::Windows::Web::Http::HttpClient m_http{ nullptr };
        winrt::hstring m_base{ L"http://127.0.0.1:8600" };
        winrt::hstring m_lastError;
    };

    // ================= 实现（header-only，量小不值得拆 .cpp） =================

    inline void ApiClient::SetToken(winrt::hstring const& token)
    {
        if (!m_http) m_http = winrt::Windows::Web::Http::HttpClient();
        m_http.DefaultRequestHeaders().Clear(); // 重登录防重复叠加
        m_http.DefaultRequestHeaders().Append(L"Authorization", L"Bearer " + std::wstring(token));
    }

    // ================= POST/PUT/外连（与 GetAsync 同套路，不重复注释） =================

    inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
        ApiClient::PostAsync(winrt::hstring const& path, winrt::hstring const& json)
    {
        using namespace winrt::Windows::Web::Http;
        m_lastError = L"";
        try
        {
            if (!m_http) m_http = HttpClient();
            HttpStringContent content(json, winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8,
                                      L"application/json");
            auto resp = co_await m_http.PostAsync(winrt::Windows::Foundation::Uri(m_base + path), content);
            co_return co_await resp.Content().ReadAsStringAsync();
        }
        catch (...) { m_lastError = L"请求失败"; co_return L""; }
    }

    inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
        ApiClient::PutAsync(winrt::hstring const& path, winrt::hstring const& json)
    {
        using namespace winrt::Windows::Web::Http;
        m_lastError = L"";
        try
        {
            if (!m_http) m_http = HttpClient();
            HttpStringContent content(json, winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8,
                                      L"application/json");
            auto resp = co_await m_http.PutAsync(winrt::Windows::Foundation::Uri(m_base + path), content);
            co_return co_await resp.Content().ReadAsStringAsync();
        }
        catch (...) { m_lastError = L"请求失败"; co_return L""; }
    }

    inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
        ApiClient::GetUrlAsync(winrt::hstring const& url)
    {
        using namespace winrt::Windows::Web::Http;
        m_lastError = L"";
        try
        {
            if (!m_http) m_http = HttpClient();
            auto resp = co_await m_http.GetAsync(winrt::Windows::Foundation::Uri(url));
            if (resp.IsSuccessStatusCode()) co_return co_await resp.Content().ReadAsStringAsync();
            m_lastError = L"HTTP " + std::to_wstring(static_cast<int>(resp.StatusCode()));
        }
        catch (...) { m_lastError = L"无法访问外部服务"; }
        co_return L"";
    }

    inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
        ApiClient::DeleteAsync(winrt::hstring const& path)
    {
        using namespace winrt::Windows::Web::Http;
        m_lastError = L"";
        try
        {
            if (!m_http) m_http = HttpClient();
            auto resp = co_await m_http.DeleteAsync(winrt::Windows::Foundation::Uri(m_base + path));
            if (resp.IsSuccessStatusCode()) co_return co_await resp.Content().ReadAsStringAsync();
            m_lastError = L"服务器错误 (" + std::to_wstring(static_cast<int>(resp.StatusCode())) + L")";
        }
        catch (...) { m_lastError = L"请求失败"; }
        co_return L"";
    }

    inline winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::Streams::IBuffer>
        ApiClient::DownloadAsync(winrt::hstring const& path)
    {
        using namespace winrt::Windows::Web::Http;
        m_lastError = L"";
        try
        {
            if (!m_http) m_http = HttpClient();
            auto resp = co_await m_http.GetAsync(winrt::Windows::Foundation::Uri(m_base + path));
            if (resp.IsSuccessStatusCode()) co_return co_await resp.Content().ReadAsBufferAsync();
            m_lastError = L"下载失败 (" + std::to_wstring(static_cast<int>(resp.StatusCode())) + L")";
        }
        catch (...) { m_lastError = L"下载失败"; }
        co_return nullptr;
    }

    inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
        ApiClient::UploadPersonalAsync(winrt::hstring const& path, winrt::hstring const& fileName,
                                       winrt::Windows::Storage::Streams::IBuffer const& data)
    {
        using namespace winrt::Windows::Web::Http;
        m_lastError = L"";
        try
        {
            if (!m_http) m_http = HttpClient();
            HttpBufferContent fc(data);
            fc.Headers().Append(L"Content-Type", L"application/octet-stream");
            HttpMultipartFormDataContent form; // C++/WinRT 保留 Http 前缀（C# 才叫 MultipartFormDataContent）
            form.Add(fc, L"file", fileName); // 带文件名的表单字段
            HttpStringContent sc(L"personal", winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8);
            form.Add(sc, L"scope");
            auto resp = co_await m_http.PostAsync(winrt::Windows::Foundation::Uri(m_base + path), form);
            if (resp.IsSuccessStatusCode()) co_return co_await resp.Content().ReadAsStringAsync();
            m_lastError = L"上传失败 (" + std::to_wstring(static_cast<int>(resp.StatusCode())) + L")";
        }
        catch (...) { m_lastError = L"上传失败"; }
        co_return L"";
    }

    inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
        ApiClient::GetAsync(winrt::hstring const& path)
    {
        using namespace winrt::Windows::Web::Http;
        m_lastError = L"";
        try
        {
            if (!m_http) m_http = HttpClient();
            auto resp = co_await m_http.GetAsync(winrt::Windows::Foundation::Uri(m_base + path));
            auto text = co_await resp.Content().ReadAsStringAsync();
            int code = static_cast<int>(resp.StatusCode());
            if (code == 200) co_return text;
            if (code == 401) m_lastError = L"登录已过期";
            else m_lastError = L"服务器错误 (" + std::to_wstring(code) + L")";
        }
        catch (winrt::hresult_error const& e)
        {
            m_lastError = L"无法连接服务器：" + e.message();
        }
        catch (...) { m_lastError = L"未知错误"; }
        co_return L"";
    }

    inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
        ApiClient::LoginAsync(winrt::hstring const& user, winrt::hstring const& pass)
    {
        using namespace winrt::Windows::Web::Http;
        using namespace winrt::Windows::Data::Json;
        m_lastError = L"";
        try
        {
            if (!m_http) m_http = HttpClient();

            JsonObject body;
            body.Insert(L"username", JsonValue::CreateStringValue(user));
            body.Insert(L"password", JsonValue::CreateStringValue(pass));
            HttpStringContent content(body.Stringify(),
                winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8, L"application/json");

            auto uri = winrt::Windows::Foundation::Uri(m_base + L"/api/auth/login");
            auto resp = co_await m_http.PostAsync(uri, content);
            auto text = co_await resp.Content().ReadAsStringAsync();
            int code = static_cast<int>(resp.StatusCode());

            if (code == 200)
            {
                auto obj = JsonObject::Parse(text);
                co_return obj.GetNamedString(L"token");
            }
            else if (code == 401) m_lastError = L"用户名或密码错误";
            else if (code == 403) m_lastError = L"账号已被禁用";
            else
            {
                std::wstring msg = L"服务器错误 (" + std::to_wstring(code) + L")";
                m_lastError = msg;
            }
        }
        catch (winrt::hresult_error const& e)
        {
            m_lastError = L"无法连接服务器：" + e.message();
        }
        catch (...)
        {
            m_lastError = L"未知错误";
        }
        co_return L"";
    }

    // ---- DPAPI + 注册表 ----
    inline void RegWriteBinary(wchar_t const* name, BYTE const* data, DWORD size)
    {
        HKEY k{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\IF-OA\\Island", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            RegSetValueExW(k, name, 0, REG_BINARY, data, size);
            RegCloseKey(k);
        }
    }

    inline bool RegReadBinary(wchar_t const* name, std::vector<BYTE>& out)
    {
        HKEY k{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\IF-OA\\Island", 0, KEY_READ, &k) != ERROR_SUCCESS)
            return false;
        DWORD size{};
        if (RegQueryValueExW(k, name, nullptr, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
        {
            RegCloseKey(k);
            return false;
        }
        out.resize(size);
        bool ok = RegQueryValueExW(k, name, nullptr, nullptr, out.data(), &size) == ERROR_SUCCESS;
        RegCloseKey(k);
        return ok;
    }

    inline void ApiClient::SaveToken(winrt::hstring const& token)
    {
        DATA_BLOB in{}, out{};
        in.pbData = const_cast<BYTE*>(reinterpret_cast<BYTE const*>(token.c_str()));
        in.cbData = static_cast<DWORD>(token.size() * sizeof(wchar_t));
        if (CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        {
            RegWriteBinary(L"token", out.pbData, out.cbData);
            LocalFree(out.pbData);
        }
    }

    inline winrt::hstring ApiClient::LoadToken()
    {
        std::vector<BYTE> blob;
        if (!RegReadBinary(L"token", blob)) return L"";
        DATA_BLOB in{}, out{};
        in.pbData = blob.data();
        in.cbData = static_cast<DWORD>(blob.size());
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return L"";
        std::wstring token(reinterpret_cast<wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
        LocalFree(out.pbData);
        return winrt::hstring(token);
    }

    inline void ApiClient::SaveServer(winrt::hstring const& url)
    {
        HKEY k{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\IF-OA\\Island", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            RegSetValueExW(k, L"server", 0, REG_SZ,
                           reinterpret_cast<BYTE const*>(url.c_str()),
                           static_cast<DWORD>((url.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
    }

    inline winrt::hstring ApiClient::LoadServer()
    {
        wchar_t buf[256]{};
        DWORD size = sizeof(buf);
        HKEY k{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\IF-OA\\Island", 0, KEY_READ, &k) == ERROR_SUCCESS)
        {
            RegQueryValueExW(k, L"server", nullptr, nullptr, reinterpret_cast<BYTE*>(buf), &size);
            RegCloseKey(k);
        }
        return buf[0] ? winrt::hstring(buf) : winrt::hstring(L"http://127.0.0.1:8600");
    }
}
