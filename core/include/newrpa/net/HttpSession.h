#pragma once
// ============================================================================
// HttpSession.h — 带会话 Cookie 的通用 HTTP 取数器（单头文件）
//
// 与 HttpDownloader 的分工：
//   HttpDownloader : 纯传输层（WinHTTP / HTTPS / 自定义 Header）
//   CookieJar      : 会话凭证的解析、持久化、按 URL 匹配
//   HttpSession    : 把两者组合，按"任务列表"批量取数并落盘
//
// 设计目标：与具体银行解耦。任何插件只要完成登录，主程序即可用
//           CDP 抓 Cookie，然后按 config.json 的 httpTasks 直连取数。
//
// 配置示例（config.json 根级）：
//   "httpTasks": [
//     { "name": "当日交易",
//       "url": "{origin}/cdn/demoYSB/AM/AM2/AM2DLZThatDay.dlz",
//       "saveAs": "HTTP_当日交易_{timestamp}.dlz" }
//   ]
//
// 占位符：{origin} {bank} {date} YYYYMMDD {timestamp} YYYYMMDD_HHMMSS
// ============================================================================

#include "CookieJar.h"
#include "HttpDownloader.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace newrpa {
namespace net {

struct HttpTask {
    std::string name;
    std::string url;
    std::string saveAs;
    std::string method = "GET";
    std::string body;
    std::map<std::string, std::string> headers;
};

class HttpSession {
public:
    void SetCookieJar(CookieJar jar)   { jar_ = std::move(jar); }
    void SetOrigin(const std::string& o) { origin_ = o; }
    void SetBank(const std::string& b) { bank_ = b; }
    void SetReferer(const std::string& r) { referer_ = r; }
    void SetIgnoreCertErrors(bool v)   { dl_.SetIgnoreCertErrors(v); }
    void SetTimeout(DWORD ms)          { dl_.SetTimeout(ms); }

    const CookieJar& cookieJar() const { return jar_; }

    // 单次请求：自动注入匹配的 Cookie + Referer + User-Agent
    DownloadResult Get(const std::string& url,
                       const std::map<std::string, std::string>& extra = {}) {
        return Do(url, "GET", "", extra);
    }
    DownloadResult Post(const std::string& url, const std::string& body,
                        const std::map<std::string, std::string>& extra = {}) {
        return Do(url, "POST", body, extra);
    }

    // 批量执行任务；返回可读摘要（由调用方写日志）
    std::string RunTasks(const std::vector<HttpTask>& tasks,
                         const std::string& downloadDir) {
        std::ostringstream out;
        std::error_code ec;
        std::filesystem::create_directories(downloadDir, ec);

        std::string date, timestamp;
        Stamp(date, timestamp);

        out << "HTTP 会话取数: " << tasks.size() << " 个任务, Cookie "
            << jar_.size() << " 条\n";

        int okCount = 0;
        for (const auto& t : tasks) {
            std::string url = Expand(t.url, date, timestamp);
            if (url.empty()) { out << t.name << ": 跳过（URL 为空）\n"; continue; }

            std::string ck = jar_.HeaderFor(url);
            out << "  [" << t.name << "] " << t.method << " " << url << "\n";
            out << "    注入 Cookie: " << (ck.empty() ? "(无匹配)" : Mask(ck)) << "\n";

            DownloadResult r = Do(url, t.method.empty() ? "GET" : t.method,
                                  t.body, t.headers);
            if (!r.ok()) {
                std::string err = r.error.empty()
                    ? ("HTTP " + std::to_string(r.status)) : r.error;
                out << "    失败: " << err << "\n";
                continue;
            }

            std::string name = t.saveAs.empty() ? DefaultFileName(url) : t.saveAs;
            name = Expand(name, date, timestamp);
            std::filesystem::path fp = std::filesystem::path(downloadDir) / name;
            std::ofstream ofs(fp, std::ios::binary | std::ios::trunc);
            if (!ofs) { out << "    落盘失败: " << fp.string() << "\n"; continue; }
            ofs.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
            ofs.close();

            out << "    OK: HTTP " << r.status << ", " << r.body.size()
                << " 字节 → " << fp.filename().string() << "\n";
            ++okCount;
        }
        out << "完成: " << okCount << "/" << tasks.size() << " 成功\n";
        return out.str();
    }

    // 从 JSON 数组读任务；也用于配置文件缺省时的兜底
    static std::vector<HttpTask> TasksFromJson(const nlohmann::json& arr) {
        std::vector<HttpTask> tasks;
        if (!arr.is_array()) return tasks;
        for (const auto& item : arr) {
            if (!item.is_object()) continue;
            HttpTask t;
            t.name   = item.value("name", "");
            t.url    = item.value("url", "");
            t.saveAs = item.value("saveAs", "");
            t.method = item.value("method", std::string("GET"));
            t.body   = item.value("body", "");
            if (item.contains("headers") && item["headers"].is_object()) {
                for (auto it = item["headers"].begin(); it != item["headers"].end(); ++it)
                    t.headers[it.key()] = it.value().get<std::string>();
            }
            if (!t.url.empty()) tasks.push_back(std::move(t));
        }
        return tasks;
    }

    // 演示站兜底任务（仅在配置里没有 httpTasks 时使用）
    static std::vector<HttpTask> DefaultTasks(const std::string& origin) {
        std::vector<HttpTask> tasks;
        tasks.push_back({"当日交易",
            origin + "/cdn/demoYSB/AM/AM2/AM2DLZThatDay.dlz",
            "HTTP_当日交易_{timestamp}.dlz"});
        tasks.push_back({"历史交易",
            origin + "/cdn/demoYSB/AM/AM2/AM2DLZHistory.dlz",
            "HTTP_历史交易_{timestamp}.dlz"});
        return tasks;
    }

private:
    HttpDownloader dl_;
    CookieJar      jar_;
    std::string    origin_;
    std::string    bank_;
    std::string    referer_;

    DownloadResult Do(const std::string& url, const std::string& method,
                      const std::string& body,
                      const std::map<std::string, std::string>& extra) {
        std::map<std::string, std::string> h;
        if (!referer_.empty()) h["Referer"] = referer_;
        std::string ck = jar_.HeaderFor(url);
        if (!ck.empty()) h["Cookie"] = ck;
        for (const auto& kv : extra) h[kv.first] = kv.second;
        if (method == "POST" && !h.count("Content-Type"))
            h["Content-Type"] = "application/x-www-form-urlencoded";
        return (method == "POST") ? dl_.Post(url, body, h) : dl_.Get(url, h);
    }

    std::string Expand(std::string s, const std::string& date,
                       const std::string& timestamp) const {
        ReplaceAll(s, "{origin}", origin_);
        ReplaceAll(s, "{bank}", bank_);
        ReplaceAll(s, "{date}", date);
        ReplaceAll(s, "{timestamp}", timestamp);
        return s;
    }

    static void ReplaceAll(std::string& s, const std::string& from,
                           const std::string& to) {
        if (from.empty()) return;
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) {
            s.replace(p, from.size(), to);
            p += to.size();
        }
    }

    static void Stamp(std::string& date, std::string& timestamp) {
        std::time_t now = std::time(nullptr);
        std::tm tmNow{};
        localtime_s(&tmNow, &now);
        char d[16] = {0}, t[32] = {0};
        std::strftime(d, sizeof(d), "%Y%m%d", &tmNow);
        std::strftime(t, sizeof(t), "%Y%m%d_%H%M%S", &tmNow);
        date = d;
        timestamp = t;
    }

    static std::string DefaultFileName(const std::string& url) {
        std::string name = "http_response";
        size_t q = url.find_first_of("?#");
        std::string u = url.substr(0, q);
        size_t slash = u.find_last_of('/');
        if (slash != std::string::npos && slash + 1 < u.size()) name = u.substr(slash + 1);
        return name;
    }

    static std::string Mask(const std::string& cookie) {
        if (cookie.size() <= 18) return cookie;
        return cookie.substr(0, 14) + "...(len=" + std::to_string(cookie.size()) + ")";
    }
};

} // namespace net
} // namespace newrpa
