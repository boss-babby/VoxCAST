#pragma once
// ============================================================================
//  VoxCast — net/HttpClient.h
//  Transport abstraction so the AI layer never names a specific HTTP stack.
//  Windows  -> WinHttpClient  (WinHTTP; no libcurl/OpenSSL to redistribute)
//  macOS    -> CurlHttpClient (libcurl, or NSURLSession in the bundle build)
// ============================================================================
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace vox::net {

struct Response {
    long        status{0};
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

struct Request {
    std::string url;
    std::string method{"POST"};
    std::string body;
    std::map<std::string, std::string> headers;
    int timeoutMs{15000};
    /// Invoked per streamed chunk. Return false to abort the transfer
    /// (used by the Esc cancellation path).
    std::function<bool(const char*, size_t)> onChunk;
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual Response send(const Request&) = 0;
    static std::unique_ptr<IHttpClient> create();
};

} // namespace vox::net
