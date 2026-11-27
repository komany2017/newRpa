#pragma once
// ============================================================================
// HttpDownloader.h — 基于 Windows WinHTTP 的轻量 HTTPS 客户端（单头文件）
//
// 零外部依赖：只用系统自带 winhttp.dll（编译链 -lwinhttp）。
// 相比原 HttpClient（httplib 强制 http://、无 SSL），本类：
//   * 原生 HTTPS，使用 Windows schannel（与 curl/Chrome 一致，兼容性好）
//   * 可忽略自签/过期证书错误（演示站必需）
//   * 支持自定义请求头（Cookie / Referer / User-Agent / token）
//   * 支持 GET / POST、文本与二进制、落盘下载
//
// 用法：
//   newrpa::net::HttpDownloader dl;
//   dl.SetIgnoreCertErrors(true);
//   auto r = dl.Get("https://.../file.dlz", {{"Cookie", cookie}});
//   if (r.ok()) { /* r.body */ }
// ============================================================================

#include <windows.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <map>
#include <vector>
#include <cstdlib>

namespace newrpa {
namespace net {

struct DownloadResult {
    int         status = 0;
    std::string body;
    std::string error;
    bool ok() const { return error.empty() && status >= 200 && status < 400; }
};

class HttpDownloader {
public:
    HttpDownloader() = default;

    void SetIgnoreCertErrors(bool v) { ignoreCert_ = v; }
    void SetTimeout(DWORD ms) { timeoutMs_ = ms; }
    void SetUserAgent(const std::string& ua) { userAgent_ = ua; }
    const std::string& lastError() const { return lastError_; }

    DownloadResult Get(const std::string& url,
                       const std::map<std::string, std::string>& headers = {}) {
        return Do(url, "GET", "", headers);
    }

    DownloadResult Post(const std::string& url,
                        const std::string& body,
                        const std::map<std::string, std::string>& headers = {}) {
        return Do(url, "POST", body, headers);
    }

    bool DownloadToFile(const std::string& url,
                        const std::string& localPath,
                        const std::map<std::string, std::string>& headers = {}) {
        DownloadResult r = Get(url, headers);
        if (!r.ok()) {
            lastError_ = r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error;
            return false;
        }
        // 用 fs::path 构造 ofstream（Unicode 路径），fopen_s 传 char* 会把
        // UTF-8 中文路径按 ANSI 代码页解读，生成的文件名会乱码。
        std::ofstream ofs(std::filesystem::path(localPath),
                          std::ios::binary | std::ios::trunc);
        if (!ofs) { lastError_ = "Cannot open file: " + localPath; return false; }
        ofs.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
        return ofs.good();
    }

private:
    bool ignoreCert_ = true;
    DWORD timeoutMs_ = 30000;
    std::string userAgent_ = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) NewRpa/1.0";
    std::string lastError_;

    static std::wstring W(const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring out(static_cast<size_t>(n > 0 ? n : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
        if (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }

    static bool ParseUrl(const std::string& url, std::string& scheme,
                         std::string& host, unsigned short& port,
                         std::string& path) {
        size_t pos = url.find("://");
        if (pos == std::string::npos) return false;
        scheme = url.substr(0, pos);
        size_t hs = pos + 3;
        size_t slash = url.find('/', hs);
        std::string hostport;
        if (slash == std::string::npos) { hostport = url.substr(hs); path = "/"; }
        else { hostport = url.substr(hs, slash - hs); path = url.substr(slash); }
        host = hostport;
        port = (scheme == "https") ? 443 : 80;
        size_t colon = hostport.find(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            port = static_cast<unsigned short>(std::atoi(hostport.c_str() + colon + 1));
        }
        return !host.empty();
    }

    DownloadResult Do(const std::string& url, const std::string& method,
                      const std::string& body,
                      const std::map<std::string, std::string>& headers) {
        DownloadResult res;
        lastError_.clear();
        std::string scheme, host, path;
        unsigned short port = 0;
        if (!ParseUrl(url, scheme, host, port, path)) {
            res.error = "Invalid URL: " + url;
            return res;
        }
        bool secure = (scheme == "https");

        HINTERNET hSession = WinHttpOpen(
            W(userAgent_).c_str(),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { res.error = "WinHttpOpen failed"; return res; }
        WinHttpSetTimeouts(hSession, static_cast<int>(timeoutMs_), static_cast<int>(timeoutMs_),
                           static_cast<int>(timeoutMs_), static_cast<int>(timeoutMs_));

        DWORD secFlags = 0x00001000 | 0x00002000 | 0x00004000 | 0x00008000;
        // WINHTTP_IGNORE_CERT_ERRORS | INVALID_CERT | WRONG_USAGE | REVOCATION
        if (ignoreCert_)
            WinHttpSetOption(hSession, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

        HINTERNET hConn = WinHttpConnect(hSession, W(host).c_str(), port, 0);
        if (!hConn) {
            res.error = "WinHttpConnect failed (err=" + std::to_string(GetLastError()) + ")";
            WinHttpCloseHandle(hSession);
            return res;
        }

        DWORD reqFlags = secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConn, W(method).c_str(), W(path).c_str(),
                                            nullptr, nullptr, nullptr, reqFlags);
        if (!hReq) {
            res.error = "WinHttpOpenRequest failed";
            WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
            return res;
        }
        if (ignoreCert_ && secure)
            WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

        // 请求头（UTF-8）
        std::string hdr;
        for (const auto& kv : headers)
            hdr += kv.first + ": " + kv.second + "\r\n";
        if (!hdr.empty()) {
            std::wstring wh = W(hdr);
            WinHttpAddRequestHeaders(hReq, wh.c_str(),
                static_cast<DWORD>(wh.size()), WINHTTP_ADDREQ_FLAG_ADD);
        }

        DWORD bodyLen = static_cast<DWORD>(body.size());
        LPVOID pBody = body.empty() ? nullptr : const_cast<char*>(body.data());
        if (!WinHttpSendRequest(hReq, nullptr, 0, pBody, bodyLen, bodyLen, 0)) {
            res.error = "WinHttpSendRequest failed (err=" + std::to_string(GetLastError()) + ")";
            WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
            return res;
        }
        if (!WinHttpReceiveResponse(hReq, nullptr)) {
            res.error = "WinHttpReceiveResponse failed";
            WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
            return res;
        }

        DWORD statusCode = 0, len = sizeof(statusCode);
        if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &len, WINHTTP_NO_HEADER_INDEX))
            res.status = static_cast<int>(statusCode);

        std::vector<char> buf(16384);
        while (true) {
            DWORD read = 0;
            if (!WinHttpReadData(hReq, buf.data(), static_cast<DWORD>(buf.size()), &read) || read == 0)
                break;
            res.body.append(buf.data(), read);
        }

        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return res;
    }
};

} // namespace net
} // namespace newrpa
