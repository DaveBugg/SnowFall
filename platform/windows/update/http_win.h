// HTTPS GET for the update check (WinHTTP). Behind a tiny interface so the updater logic can be
// tested offline with a fake transport and ui_demo never touches the network.
//
// Privacy: one plain GET per call. Headers: User-Agent "SnowFall/<version>" and Accept only; no
// cookies, no credentials (automatic logon disabled, authentication challenges are not
// answered), no referer. Redirects are followed manually and every target must pass
// update::isAllowedUrl().
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace snow::update {

struct HttpResult {
    bool ok = false;           // transport worked, HTTP 200 and the whole body was delivered
    int status = 0;            // last HTTP status (0 = no response)
    std::size_t bytes = 0;     // body bytes delivered to the sink
    unsigned long error = 0;   // Win32 / WinHTTP error code when !ok and status == 0
    std::string reason;        // short diagnostic ("redirect not allowed", "too large", ...)
};

// Receives the body in chunks; `total` = Content-Length or -1. Return false to abort.
using HttpSink = std::function<bool(const std::uint8_t* data, std::size_t size, long long total)>;

enum class Accept { GitHubJson, Binary };

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    // Blocking GET of `url` (must pass isAllowedUrl). Bodies larger than maxBytes fail.
    virtual HttpResult get(const std::string& url, Accept accept, std::size_t maxBytes, const HttpSink& sink) = 0;
    // From any thread: aborts the request in progress (it fails) and every later one.
    virtual void cancel() = 0;
};

// WinHTTP implementation; userAgent e.g. "SnowFall/0.1.2+abc1234". Timeouts 15 s.
std::unique_ptr<HttpTransport> makeWinHttpTransport(const std::string& userAgent);

}  // namespace snow::update
