#include "update/http_win.h"

#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <vector>

#include "diagnostics/log.h"
#include "update/update.h"

#pragma comment(lib, "winhttp.lib")

namespace snow::update {

namespace {

constexpr DWORD kTimeoutMs = 15000;
constexpr int kMaxRedirects = 5;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// "https://host/path?query" -> "host" (for the log: redirect targets carry signed query strings).
std::string hostOf(const std::string& url) {
    const size_t start = url.find("://");
    if (start == std::string::npos) return "?";
    const size_t end = url.find_first_of("/?#", start + 3);
    return url.substr(start + 3, end == std::string::npos ? std::string::npos : end - start - 3);
}

class WinHttpTransport final : public HttpTransport {
public:
    explicit WinHttpTransport(const std::string& userAgent) : userAgent_(widen(userAgent)) {}

    ~WinHttpTransport() override {
        cancel();
        if (session_) WinHttpCloseHandle(session_);
    }

    void cancel() override {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        if (request_) {
            WinHttpCloseHandle(request_);   // a blocking call on the worker thread fails at once
            request_ = nullptr;
        }
    }

    HttpResult get(const std::string& startUrl, Accept accept, std::size_t maxBytes, const HttpSink& sink) override {
        HttpResult r;
        if (!ensureSession()) {
            r.error = GetLastError();
            r.reason = "session";
            return r;
        }
        std::string url = startUrl;
        for (int hop = 0; hop <= kMaxRedirects; ++hop) {
            if (!isAllowedUrl(url)) {
                r.reason = "url not allowed";
                SNOW_LOGW("update: refused a request to %s (not an allowed URL)", hostOf(url).c_str());
                return r;
            }
            std::string location;
            if (!requestOnce(url, accept, maxBytes, sink, r, location)) return r;
            if (location.empty()) return r;   // final response (ok or not)
            SNOW_LOGI("update: HTTP %d redirect to %s", r.status, hostOf(location).c_str());
            url = location;
        }
        r.ok = false;
        r.reason = "too many redirects";
        return r;
    }

private:
    bool ensureSession() {
        if (session_) return true;
        session_ = WinHttpOpen(userAgent_.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) return false;
        WinHttpSetTimeouts(session_, static_cast<int>(kTimeoutMs), static_cast<int>(kTimeoutMs), static_cast<int>(kTimeoutMs),
                           static_cast<int>(kTimeoutMs));
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;   // older systems without TLS 1.3 support
            WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
        return true;
    }

    // One request without following redirects. Returns false when the result is final and
    // failed; true with `location` set for a redirect, or true with r.ok for a 200 body.
    bool requestOnce(const std::string& url, Accept accept, std::size_t maxBytes, const HttpSink& sink, HttpResult& r,
                     std::string& location) {
        r = HttpResult{};
        const std::wstring wurl = widen(url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof uc;
        wchar_t host[256] = {};
        uc.lpszHostName = host;
        uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
        uc.dwUrlPathLength = static_cast<DWORD>(-1);
        uc.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
            r.error = GetLastError();
            r.reason = "bad url";
            return false;
        }
        std::wstring path(uc.lpszUrlPath ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : std::wstring(L"/"));
        if (uc.lpszExtraInfo && uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
        if (path.empty()) path = L"/";

        HINTERNET connect = WinHttpConnect(session_, host, uc.nPort, 0);
        if (!connect) {
            r.error = GetLastError();
            r.reason = "connect";
            return false;
        }
        HINTERNET req = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) {
            r.error = GetLastError();
            r.reason = "open request";
            WinHttpCloseHandle(connect);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_) {
                WinHttpCloseHandle(req);
                WinHttpCloseHandle(connect);
                r.reason = "cancelled";
                return false;
            }
            request_ = req;
        }
        // No cookies, no automatic redirects (checked by hand), no authentication / credentials.
        DWORD features = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
        DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        WinHttpSetOption(req, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof logon);
        const wchar_t* headers = accept == Accept::GitHubJson ? L"Accept: application/vnd.github+json\r\n"
                                                              : L"Accept: application/octet-stream\r\n";

        bool result = false;
        if (!WinHttpSendRequest(req, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr)) {
            r.error = GetLastError();
            r.reason = "no response";
        } else {
            DWORD status = 0, len = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &len, WINHTTP_NO_HEADER_INDEX);
            r.status = static_cast<int>(status);
            if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                wchar_t buf[4096] = {};
                DWORD blen = sizeof buf - sizeof(wchar_t);
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, buf, &blen,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    location = narrow(buf);
                    if (!location.empty() && location[0] == '/') location = "https://" + narrow(host) + location;
                    result = !location.empty();
                }
                if (!result) r.reason = "redirect without location";
            } else if (status != 200) {
                r.reason = "http status";
            } else {
                result = readBody(req, maxBytes, sink, r);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // request_ only becomes null through cancel() (which closed the handle).
            const bool wasCancelled = cancelled_ || request_ == nullptr;
            if (request_) {
                WinHttpCloseHandle(request_);
                request_ = nullptr;
            }
            if (wasCancelled) {
                r.ok = false;
                r.reason = "cancelled";
                result = false;
            }
        }
        WinHttpCloseHandle(connect);
        return result;
    }

    bool readBody(HINTERNET req, std::size_t maxBytes, const HttpSink& sink, HttpResult& r) {
        long long total = -1;
        ULONGLONG cl = 0;
        DWORD len = sizeof cl;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64, WINHTTP_HEADER_NAME_BY_INDEX, &cl,
                                &len, WINHTTP_NO_HEADER_INDEX)) {
            total = static_cast<long long>(cl);
            if (cl > maxBytes) {
                r.reason = "too large";
                return false;
            }
        }
        std::vector<std::uint8_t> buf(64 * 1024);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
                r.error = GetLastError();
                r.reason = "read";
                return false;
            }
            if (got == 0) break;
            if (r.bytes + got > maxBytes) {
                r.reason = "too large";
                return false;
            }
            r.bytes += got;
            if (sink && !sink(buf.data(), got, total)) {
                r.reason = "aborted";
                return false;
            }
        }
        if (total >= 0 && static_cast<long long>(r.bytes) != total) {
            r.reason = "truncated";
            return false;
        }
        r.ok = true;
        return true;
    }

    std::wstring userAgent_;
    HINTERNET session_ = nullptr;
    std::mutex mutex_;
    HINTERNET request_ = nullptr;
    bool cancelled_ = false;
};

}  // namespace

std::unique_ptr<HttpTransport> makeWinHttpTransport(const std::string& userAgent) {
    return std::make_unique<WinHttpTransport>(userAgent);
}

}  // namespace snow::update
