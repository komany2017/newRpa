// ============================================================================
// Config / HttpClient / ExcelWriter / Crypto / Screenshot 实现
// ============================================================================

#include "newrpa/util/Config.h"
#include "newrpa/net/HttpClient.h"
#include "newrpa/io/ExcelWriter.h"
#include "newrpa/util/StringUtils.h"
#include "newrpa/cdp/CDPSession.h"
#include "newrpa/crypto/Crypto.h"
#include "newrpa/util/ConfigDb.h"

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

// 解析配置 JSON 文本 → GlobalConfig（内部函数）
static GlobalConfig ParseConfigJson(const std::string& jsonText) {
    GlobalConfig cfg;
    json j;
    try { j = json::parse(jsonText); }
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

    // 加解密配置（顶层 crypto 段）：AES 密钥直接配置，RSA 密钥从文件读取
    if (j.contains("crypto") && j["crypto"].is_object()) {
        auto& c = j["crypto"];
        cfg.aesKeyHex         = c.value("aesKey",            "");
        cfg.rsaPrivateKeyFile = c.value("rsaPrivateKeyFile", "");
        cfg.rsaPublicKeyFile  = c.value("rsaPublicKeyFile",  "");
        spdlog::info("[Config] crypto section: aesKey={}, rsaKeys={}",
                     cfg.aesKeyHex.empty() ? "MISSING" : "set",
                     (cfg.rsaPrivateKeyFile.empty() || cfg.rsaPublicKeyFile.empty())
                         ? "MISSING" : "set");
    }

    // 超级鹰打码（公共，凭据不进日志）
    if (j.contains("ocr") && j["ocr"].is_object()) {
        auto& o = j["ocr"];
        cfg.ocrUser     = o.value("user",     "");
        cfg.ocrPass     = o.value("pass",     "");
        cfg.ocrSoftId   = o.value("softid",   "");
        cfg.ocrUrl      = o.value("url",      "");
        cfg.ocrCodeType = o.value("codetype", "");
        spdlog::info("[Config] ocr section: {}",
                     (cfg.ocrUser.empty() || cfg.ocrPass.empty() ||
                      cfg.ocrSoftId.empty())
                         ? "incomplete (env CJY_* can fill gaps)"
                         : "configured");
    }

    // Chrome 启动参数 / prefs / excludeSwitches
    if (j.contains("chromeArgs") && j["chromeArgs"].is_array()) {
        for (auto& a : j["chromeArgs"])
            if (a.is_string()) cfg.chromeArgs.push_back(a.get<std::string>());
        spdlog::info("[Config] chromeArgs: {} item(s)", cfg.chromeArgs.size());
    }
    // 解压版扩展目录（--load-extension）
    if (j.contains("extensionPaths") && j["extensionPaths"].is_array()) {
        for (auto& e : j["extensionPaths"])
            if (e.is_string()) cfg.extensionPaths.push_back(e.get<std::string>());
        spdlog::info("[Config] extensionPaths: {} item(s)", cfg.extensionPaths.size());
    }
    if (j.contains("chromePrefs")) {
        cfg.chromePrefsJson = j["chromePrefs"].dump();
        spdlog::info("[Config] chromePrefs: {} bytes", cfg.chromePrefsJson.size());
    }
    if (j.contains("chromeExcludeSwitches") && j["chromeExcludeSwitches"].is_array()) {
        for (auto& s : j["chromeExcludeSwitches"])
            if (s.is_string()) cfg.chromeExcludeSwitches.push_back(s.get<std::string>());
    }

    // 银行登录密码 / 证书密码支持 "enc:v1:" 加密存储（AES-256-CBC PKCS7）。
    // 主密钥来自 config.json 顶层 crypto.aesKey（不再用 crypto.key 文件）。
    // 明文密码仍完全兼容（无前缀原样放行）。明文值不落日志。
    const std::string aesKeyRaw = crypto::ParseAesKeyHex(cfg.aesKeyHex);

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
        bc.chromeProfileDir = b.value("chromeProfileDir", "");
        bc.chromePort       = b.value("chromePort",   0);
        // "enc:v1:" 密文 → 运行时解密为明文（只进内存，不落日志）
        if (crypto::IsEncryptedPassword(bc.password))
            bc.password = crypto::DecryptPassword(bc.password, aesKeyRaw);
        if (crypto::IsEncryptedPassword(bc.certPassword))
            bc.certPassword = crypto::DecryptPassword(bc.certPassword, aesKeyRaw);
        cfg.banks.push_back(std::move(bc));
    }
    return cfg;
}

// 从加密 sqlite3 读取配置（配置存储唯一来源）
GlobalConfig LoadConfig(const std::string& path) {
    std::string jsonText, err;
    if (!ConfigDbRead(path, jsonText, err)) {
        spdlog::error("[Config] cannot read encrypted config db '{}': {}", path, err);
        return GlobalConfig{};
    }
    spdlog::info("[Config] loaded encrypted config db: {}", path);
    return ParseConfigJson(jsonText);
}

// 写一份默认加密配置库
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
        {"ocr", {
            {"user", ""},
            {"pass", ""},
            {"softid", ""},
            {"url", ""},
            {"codetype", "1004"}
        }},
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
    std::string err;
    if (!ConfigDbWrite(path, j.dump(2), err))
        spdlog::error("[Config] cannot write encrypted config db: {}", err);
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
    std::string host, path, scheme = "http://";
    if (url.find("https://") == 0) {
        host = url.substr(8);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        scheme = "https://";
#else
        // 当前构建未链 OpenSSL（httplib 无 SSL），https 只能降级成 http
        spdlog::warn("[HttpClient] https not supported in this build (no OpenSSL), "
                     "downgrade to http: {}", url);
#endif
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

    httplib::Client cli(scheme + host);
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
// Crypto — Md5 自实现（RFC 1321，超级鹰 pass2 用）；Sha256/AES 见 src/crypto/Crypto.cpp（Botan）
// ----------------------------------------------------------------------------
#include "newrpa/crypto/Crypto.h"

namespace newrpa {
namespace crypto {

namespace {

inline uint32_t Md5Rotl(uint32_t x, int c) {
    return (x << c) | (x >> (32 - c));
}

// K[i] = floor(abs(sin(i+1)) * 2^32) 的标准常数表
const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

const int kMd5S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

} // namespace

std::string Md5(const std::string& msg) {
    std::string data = msg;
    const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56) data.push_back('\0');
    for (int i = 0; i < 8; ++i)
        data.push_back(static_cast<char>((bitLen >> (8 * i)) & 0xFF));

    uint32_t h0 = 0x67452301, h1 = 0xefcdab89,
             h2 = 0x98badcfe, h3 = 0x10325476;

    for (size_t off = 0; off < data.size(); off += 64) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data()) + off;
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = p[i * 4] | (p[i * 4 + 1] << 8) |
                   (p[i * 4 + 2] << 16) | (uint32_t(p[i * 4 + 3]) << 24);

        uint32_t A = h0, B = h1, C = h2, D = h3;
        for (int i = 0; i < 64; ++i) {
            uint32_t F;
            int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3 * i + 5) % 16; }
            else             { F = C ^ (B | ~D);       g = (7 * i) % 16; }
            F += A + kMd5K[i] + M[g];
            A = D; D = C; C = B;
            B += Md5Rotl(F, kMd5S[i]);
        }
        h0 += A; h1 += B; h2 += C; h3 += D;
    }

    // 摘要 4 个字按小端字节序逐字节输出 32 位 hex
    const uint32_t hs[4] = { h0, h1, h2, h3 };
    char out[33] = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            snprintf(out + (i * 4 + j) * 2, 3, "%02x",
                     (hs[i] >> (8 * j)) & 0xFF);
    return std::string(out);
}

    // Sha256 / AesEncrypt / AesDecrypt 已移至 core/src/crypto/Crypto.cpp（Botan 实现）

} // namespace crypto
} // namespace newrpa
