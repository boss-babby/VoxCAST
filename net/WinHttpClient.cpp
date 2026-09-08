// ============================================================================
//  VoxCast — net/WinHttpClient.cpp
//  WinHTTP transport. Chosen over libcurl on Windows because it ships with the
//  OS: no OpenSSL to vendor, no CA bundle to keep current, and it honours the
//  system proxy and certificate store that enterprise users already trust.
//  Supports chunked read-back so streamed partials surface as they arrive.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include "HttpClient.h"

#include <vector>
#include <mutex>

namespace vox::net {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h{nullptr};
    Handle() = default;
    ~Handle() { reset(); }
    void reset(HINTERNET next = nullptr) {
        if (h) WinHttpCloseHandle(h);
        h = next;
    }
    operator HINTERNET() const { return h; }
};

} // namespace

class WinHttpClient final : public IHttpClient {
public:
    WinHttpClient() {
        session_.reset(WinHttpOpen(L"VoxCast/1.0",
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    }

    Response send(const Request& req) override {
        Response out;

        if (!session_.h) {
            session_.reset(WinHttpOpen(L"VoxCast/1.0",
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        }
        if (!session_.h) { out.error = "WinHttpOpen failed"; return out; }

        WinHttpSetTimeouts(session_.h, req.timeoutMs, req.timeoutMs,
                           req.timeoutMs, req.timeoutMs);

        // --- split the URL --------------------------------------------------
        std::wstring wurl = widen(req.url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256]{}, path[4096]{};
        uc.lpszHostName = host;      uc.dwHostNameLength = 255;
        uc.lpszUrlPath  = path;      uc.dwUrlPathLength  = 4095;
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
            out.error = "malformed url"; return out;
        }

        std::wstring headerBlock;
        for (const auto& [k, v] : req.headers)
            headerBlock += widen(k) + L": " + widen(v) + L"\r\n";

        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        Handle reqh;

        // Try sending using cached connection (HTTP keep-alive)
        bool sent = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            {
                std::lock_guard lk(mtx_);
                if (!conn_.h || cachedHost_ != host || cachedPort_ != uc.nPort) {
                    conn_.reset(WinHttpConnect(session_.h, host, uc.nPort, 0));
                    cachedHost_ = host;
                    cachedPort_ = uc.nPort;
                }
            }

            if (!conn_.h) { out.error = "WinHttpConnect failed"; return out; }

            reqh.reset(WinHttpOpenRequest(conn_.h, widen(req.method).c_str(), path,
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
            if (!reqh.h) { out.error = "WinHttpOpenRequest failed"; return out; }

            sent = WinHttpSendRequest(reqh.h,
                    headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                    headerBlock.empty() ? 0 : DWORD(-1),
                    req.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)req.body.data(),
                    DWORD(req.body.size()), DWORD(req.body.size()), 0);

            if (sent) break;

            // Stale keep-alive connection closed by server; reset connection and retry once
            std::lock_guard lk(mtx_);
            conn_.reset();
        }

        if (!sent) {
            out.error = "send failed (" + std::to_string(GetLastError()) + ")";
            return out;
        }

        if (!WinHttpReceiveResponse(reqh.h, nullptr)) {
            out.error = "no response (" + std::to_string(GetLastError()) + ")";
            return out;
        }

        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(reqh.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        out.status = long(code);

        std::vector<char> buf(16384);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(reqh.h, &avail) || avail == 0) break;
            DWORD want = DWORD(std::min<size_t>(avail, buf.size()));
            DWORD got = 0;
            if (!WinHttpReadData(reqh.h, buf.data(), want, &got) || got == 0) break;
            if (req.onChunk && !req.onChunk(buf.data(), got)) {
                out.error = "cancelled";
                break;
            }
            out.body.append(buf.data(), got);
        }
        return out;
    }

private:
    std::mutex mtx_;
    Handle session_;
    Handle conn_;
    std::wstring cachedHost_;
    INTERNET_PORT cachedPort_{0};
};

std::unique_ptr<IHttpClient> IHttpClient::create() {
    return std::make_unique<WinHttpClient>();
}

} // namespace vox::net
