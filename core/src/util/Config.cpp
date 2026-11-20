// ============================================================================
// Config / HttpClient / ExcelWriter / Crypto / Screenshot 实现
// ============================================================================

#include "newrpa/util/Config.h"
#include "newrpa/net/HttpClient.h"
#include "newrpa/io/ExcelWriter.h"
#include "newrpa/util/StringUtils.h"
#include "newrpa/cdp/CDPSession.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <filesystem>
#include <sstream>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------

namespace newrpa {
namespace util {

GlobalConfig LoadConfig(const std::string& path) {
    GlobalConfig cfg;
    std::ifstream in(path);
    if (!in.is_open()) {
        spdlog::warn("[Config] file not found: {}, using defaults", path);
        return cfg;
    }
    json j;
    try { in >> j; }
    catch (const std::exception& e) {
        spdlog::error("[Config] parse error: {}", e.what());
        return cfg;
    }

    cfg.chromePath      = j.value("chromePath",      "");
    cfg.chromePort      = j.value("chromePort",      9222);
    cfg.chromeProfileDir = j.value("chromeProfileDir", "");
    cfg.headless        = j.value("headless",        false);
    cfg.pluginDir       = j.value("pluginDir",       "./plugins");
    cfg.heartbeatUrl    = j.value("heartbeatUrl",    "");
    cfg.heartbeatIntervalSec = j.value("heartbeatIntervalSec", 3600);
    cfg.logLevel        = j.value("logLevel",        "info");
    cfg.logFile         = j.value("logFile",         "");

    // Chrome 启动参数 / prefs / excludeSwitches
    if (j.contains("chromeArgs") && j["chromeArgs"].is_array()) {
        for (auto& a : j["chromeArgs"])
            if (a.is_string()) cfg.chromeArgs.push_back(a.get<std::string>());
        spdlog::info("[Config] chromeArgs: {} item(s)", cfg.chromeArgs.size());
    }
    if (j.contains("chromePrefs")) {
        cfg.chromePrefsJson = j["chromePrefs"].dump();
        spdlog::info("[Config] chromePrefs: {} bytes", cfg.chromePrefsJson.size());
    }
    if (j.contains("chromeExcludeSwitches") && j["chromeExcludeSwitches"].is_array()) {
        for (auto& s : j["chromeExcludeSwitches"])
            if (s.is_string()) cfg.chromeExcludeSwitches.push_back(s.get<std::string>());
    }

    auto banks = j.value("banks", json::array());
    for (auto& b : banks) {
        plugin::BankConfig bc;
        bc.bankName     = b.value("bankName",     "");
        bc.displayName  = b.value("displayName",  bc.bankName);
        bc.loginUrl     = b.value("loginUrl",     "");
        bc.operatorId   = b.value("operatorId",   "");
        bc.account      = b.value("account",      "");
        bc.password     = b.value("password",     "");
        bc.certPassword = b.value("certPassword", "");
        bc.ukeyType     = b.value("ukeyType",     "0");
        bc.ukeyPort     = b.value("ukeyPort",     "");
        bc.vid          = b.value("vid",          "");
        bc.pid          = b.value("pid",          "");
        bc.route        = b.value("route",        "");
        bc.dev          = b.value("dev",          "");
        bc.section      = b.value("section",      "");
        bc.company      = b.value("company",      "");
        bc.routeType    = b.value("routeType",    "");
        bc.bankType     = b.value("bankType",     "");
        cfg.banks.push_back(std::move(bc));
    }
    return cfg;
}

void SaveDefaultConfig(const std::string& path) {
    json j = {
        {"chromePath", ""},
        {"chromePort", 9222},
        {"chromeProfileDir", ""},
        {"headless", false},
        {"pluginDir", "./plugins"},
        {"heartbeatUrl", ""},
        {"heartbeatIntervalSec", 3600},
        {"logLevel", "info"},
        {"logFile", ""},
        {"banks", json::array({
            {
                {"bankName", "ICBC"},
                {"displayName", "中国工商银行"},
                {"loginUrl", "https://corporbank-simp.icbc.com.cn/icbc/normalbank/index.jsp"},
                {"operatorId", ""},
                {"account", ""},
                {"password", ""},
                {"certPassword", ""},
                {"ukeyType", "0"},
                {"ukeyPort", ""},
                {"vid", ""}, {"pid", "0100"},
                {"route", "1"}, {"dev", "2"},
                {"section", ""}, {"company", ""},
                {"bankType", "102"}, {"note", ""}
            }
        })}
    };
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path);
    out << j.dump(2);
}

} // namespace util

// ----------------------------------------------------------------------------
// HttpClient
// ----------------------------------------------------------------------------

namespace net {

HttpResponse HttpClient::Get(const std::string& url,
                              const std::map<std::string, std::string>& headers) {
    return Post(url, "", "", headers);
}

HttpResponse HttpClient::Post(const std::string& url,
                               const std::string& body,
                               const std::string& contentType,
                               const std::map<std::string, std::string>& headers) {
    HttpResponse r;
    std::string host, path;
    if (url.find("https://") == 0) {
        host = url.substr(8);
    } else if (url.find("http://") == 0) {
        host = url.substr(7);
    } else {
        r.error = "Invalid URL";
        return r;
    }
    auto slash = host.find('/');
    if (slash == std::string::npos) {
        path = "/";
    } else {
        path = host.substr(slash);
        host = host.substr(0, slash);
    }

    auto t0 = std::chrono::steady_clock::now();

    httplib::Client cli("http://" + host);
    cli.set_connection_timeout(connectTimeoutSec_);

    httplib::Headers hdrs;
    for (auto& kv : headers) hdrs.emplace(kv.first, kv.second);
    if (!contentType.empty() && hdrs.find("Content-Type") == hdrs.end())
        hdrs.emplace("Content-Type", contentType);

    httplib::Result result;
    if (body.empty()) {
        result = cli.Get(path, hdrs);
    } else {
        result = cli.Post(path, hdrs, body, contentType);
    }

    auto t1 = std::chrono::steady_clock::now();
    r.elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!result) {
        r.error = httplib::to_string(result.error());
        return r;
    }
    r.status = result->status;
    r.body   = std::move(result->body);
    for (auto& kv : result->headers)
        r.headers[kv.first] = kv.second;
    return r;
}

std::string HttpClient::GetJson(const std::string& url) {
    auto r = Get(url);
    return r.body;
}

bool HttpClient::DownloadToFile(const std::string& url,
                                const std::string& localPath) {
    auto r = Get(url);
    if (r.status != 200) return false;
    fs::create_directories(fs::path(localPath).parent_path());
    std::ofstream out(localPath, std::ios::binary);
    if (!out) return false;
    out.write(r.body.data(), r.body.size());
    return true;
}

} // namespace net

// ----------------------------------------------------------------------------
// ExcelWriter — CSV 版
// ----------------------------------------------------------------------------

namespace io {

static std::string CsvEscape(const std::string& s) {
    std::string out = s;
    bool needQuote = s.find(',') != std::string::npos ||
                     s.find('"') != std::string::npos ||
                     s.find('\n') != std::string::npos;
    // 去掉 BOM 如果有
    if (!out.empty() && (uint8_t)out[0] == 0xEF &&
        out.size() > 2 &&
        (uint8_t)out[1] == 0xBB && (uint8_t)out[2] == 0xBF)
        out = out.substr(3);
    if (needQuote) {
        out = "\"" + util::ReplaceAll(out, "\"", "\"\"") + "\"";
    }
    return out;
}

std::string WriteTransactionsCsv(
    const std::string& path,
    const std::vector<plugin::Transaction>& rows) {

    std::ofstream out(path, std::ios::binary);
    if (!out) return "";
    // UTF-8 BOM，Excel 能识别
    out.write("\xEF\xBB\xBF", 3);

    out << "流水号,账号,对方账号,对方户名,金额,方向,"
           "交易日期,交易后余额,备注,币种\n";
    for (auto& r : rows) {
        out << CsvEscape(r.sn)          << ","
            << CsvEscape(r.account)    << ","
            << CsvEscape(r.opAccount)   << ","
            << CsvEscape(r.opAccountName) << ","
            << CsvEscape(r.amount)      << ","
            << CsvEscape(r.direction)   << ","
            << CsvEscape(r.transDate)   << ","
            << CsvEscape(r.balanceAfter) << ","
            << CsvEscape(r.remark)      << ","
            << CsvEscape(r.currency)    << "\n";
    }
    return path;
}

std::string WriteCurBalCsv(
    const std::string& path,
    const std::vector<std::map<std::string, std::string>>& rows) {

    std::ofstream out(path, std::ios::binary);
    if (!out) return "";
    out.write("\xEF\xBB\xBF", 3);

    if (rows.empty()) return path;
    // 取第一行的 key 当表头
    std::vector<std::string> keys;
    for (auto& kv : rows.front()) keys.push_back(kv.first);
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) out << ",";
        out << CsvEscape(keys[i]);
    }
    out << "\n";
    for (auto& row : rows) {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) out << ",";
            auto it = row.find(keys[i]);
            out << (it == row.end() ? "" : CsvEscape(it->second));
        }
        out << "\n";
    }
    return path;
}

} // namespace io

} // namespace newrpa

// ----------------------------------------------------------------------------
// Crypto — 桩（等 OpenSSL 3.x 编好了再填）
// ----------------------------------------------------------------------------
#include "newrpa/crypto/Crypto.h"

namespace newrpa {
namespace crypto {

std::string Md5(const std::string& s) {
    spdlog::warn("[Crypto] Md5 not implemented (stub) — open lib link to OpenSSL 3.x");
    return "";
}

std::string Sha256(const std::string& s) {
    spdlog::warn("[Crypto] Sha256 not implemented (stub)");
    return "";
}

std::string AesEncrypt(const std::string& plain,
                        const std::string& key,
                        const std::string& iv) {
    spdlog::warn("[Crypto] AesEncrypt not implemented (stub)");
    return "";
}

std::string AesDecrypt(const std::string& cipher,
                        const std::string& key,
                        const std::string& iv) {
    spdlog::warn("[Crypto] AesDecrypt not implemented (stub)");
    return "";
}

} // namespace crypto
} // namespace newrpa
