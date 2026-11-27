#pragma once
// ============================================================================
// CookieJar.h — 通用会话 Cookie 容器（单头文件，无系统库依赖）
//
// 用途：CDP 登录后用 Network.getCookies / Storage.getCookies 抓 Cookie，
//       持久化到 JSON，后续 HTTP 请求按 RFC 6265 规则自动匹配并注入 Cookie 头。
//
// 关键设计：
//   * 支持 CDP getCookies 的两种返回形态（数组 / {"cookies":[...]}）
//   * domain 匹配：精确匹配、前导点后缀匹配（.example.com 命中 a.example.com）
//   * path 匹配：前缀匹配，且要求边界是 '/'（避免 /foo 命中 /foobar）
//   * secure Cookie 只注入 https；可过滤 HttpOnly / Session Cookie
//   * 不做加密：文件仅保存会话 Cookie，建议放 state/ 并加入 .gitignore
//
// 用法：
//   auto j = session->Call("Network.getCookies", json::object(), 5000);
//   auto jar = newrpa::net::CookieJar::FromCdp(j);
//   jar.Save("state/cookies_BCB.json");
//   std::string cookie = jar.HeaderFor("https://example.com/a/b");
// ============================================================================

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace newrpa {
namespace net {

struct CookieEntry {
    std::string name;
    std::string value;
    std::string domain;
    std::string path = "/";
    bool        secure   = false;
    bool        httpOnly = false;
    double      expires  = -1.0;   // CDP 秒级 epoch；-1 = 会话 Cookie
};

class CookieJar {
public:
    CookieJar() = default;

    // ---- 构造 ---------------------------------------------------------------
    static CookieJar FromCdp(const nlohmann::json& j) {
        CookieJar jar;
        const nlohmann::json* arr = nullptr;
        if (j.is_array()) arr = &j;
        else if (j.is_object() && j.contains("cookies") && j["cookies"].is_array())
            arr = &j["cookies"];
        if (!arr) return jar;

        for (const auto& item : *arr) {
            if (!item.is_object()) continue;
            CookieEntry c;
            c.name     = item.value("name", "");
            if (c.name.empty()) continue;
            c.value    = item.value("value", "");
            c.domain   = item.value("domain", "");
            c.path     = item.value("path", std::string("/"));
            c.secure   = item.value("secure", false);
            c.httpOnly = item.value("httpOnly", false);
            if (item.contains("expires") && item["expires"].is_number())
                c.expires = item["expires"].get<double>();
            jar.cookies_.push_back(std::move(c));
        }
        return jar;
    }

    // ---- 持久化 -------------------------------------------------------------
    nlohmann::json ToJson() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : cookies_) {
            arr.push_back({
                {"name", c.name}, {"value", c.value},
                {"domain", c.domain}, {"path", c.path},
                {"secure", c.secure}, {"httpOnly", c.httpOnly},
                {"expires", c.expires}
            });
        }
        nlohmann::json out;
        out["schema"]  = "newrpa-cookiejar-1";
        out["count"]   = static_cast<int>(cookies_.size());
        out["cookies"] = arr;
        return out;
    }

    bool Save(const std::string& path) const {
        try {
            // 传 fs::path 走 Unicode 路径，避免中文路径被 ANSI 代码页解读成乱码
            std::ofstream ofs(std::filesystem::path(path),
                              std::ios::binary | std::ios::trunc);
            if (!ofs) return false;
            ofs << ToJson().dump(2);
            return ofs.good();
        } catch (...) { return false; }
    }

    bool Load(const std::string& path) {
        try {
            std::ifstream ifs(std::filesystem::path(path), std::ios::binary);
            if (!ifs) return false;
            nlohmann::json j;
            ifs >> j;
            auto loaded = FromCdp(j);
            if (loaded.empty()) return false;
            cookies_ = std::move(loaded.cookies_);
            return true;
        } catch (...) { return false; }
    }

    // ---- 查询 ---------------------------------------------------------------
    size_t size() const { return cookies_.size(); }
    bool   empty() const { return cookies_.empty(); }
    void   Clear() { cookies_.clear(); }
    const std::vector<CookieEntry>& cookies() const { return cookies_; }

    // 按 RFC 6265 匹配 URL，拼成 "a=1; b=2" 形式的 Cookie 请求头。
    std::string HeaderFor(const std::string& url) const {
        std::string scheme, host, path;
        unsigned short port = 0;
        if (!ParseUrl(url, scheme, host, port, path)) return "";
        bool secureUrl = (scheme == "https");
        std::string lowHost = Lower(host);

        std::vector<std::string> parts;
        parts.reserve(cookies_.size());
        for (const auto& c : cookies_) {
            if (c.secure && !secureUrl) continue;
            if (!DomainMatch(lowHost, c.domain)) continue;
            if (!PathMatch(path, c.path)) continue;
            parts.push_back(c.name + "=" + c.value);
        }
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) out += "; ";
            out += parts[i];
        }
        return out;
    }

    // 仅返回匹配某 URL 的 Cookie（用于调试/抓包对照）
    CookieJar FilterFor(const std::string& url) const {
        CookieJar jar;
        std::string scheme, host, path;
        unsigned short port = 0;
        if (!ParseUrl(url, scheme, host, port, path)) return jar;
        bool secureUrl = (scheme == "https");
        std::string lowHost = Lower(host);
        for (const auto& c : cookies_) {
            if (c.secure && !secureUrl) continue;
            if (!DomainMatch(lowHost, c.domain)) continue;
            if (!PathMatch(path, c.path)) continue;
            jar.cookies_.push_back(c);
        }
        return jar;
    }

private:
    std::vector<CookieEntry> cookies_;

    static std::string Lower(std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }

    static bool DomainMatch(const std::string& lowHost, const std::string& domain) {
        if (domain.empty() || lowHost.empty()) return false;
        std::string d = Lower(domain);
        if (d.front() == '.') {
            std::string suffix = d.substr(1);
            if (suffix.empty()) return false;
            return lowHost.size() >= suffix.size() &&
                   lowHost.compare(lowHost.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
        return lowHost == d;
    }

    static bool PathMatch(const std::string& reqPath, const std::string& cookiePath) {
        if (cookiePath.empty() || cookiePath == "/") return true;
        std::string p = reqPath.empty() ? std::string("/") : reqPath;
        if (p.compare(0, cookiePath.size(), cookiePath) != 0) return false;
        if (p.size() == cookiePath.size()) return true;
        // 边界必须是 '/'，避免 /foo 命中 /foobar
        return cookiePath.back() == '/' || p[cookiePath.size()] == '/';
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
        port = (scheme == "https") ? 443 : 80;
        size_t colon = hostport.find(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            port = static_cast<unsigned short>(std::atoi(hostport.c_str() + colon + 1));
        } else host = hostport;
        return !host.empty();
    }
};

} // namespace net
} // namespace newrpa
