// ============================================================================
// ChaojiyingOcr 实现 — 超级鹰打码（公共 OCR 模块）
//
// 上传走 net::HttpClient（httplib），multipart 表单手工拼装，
// 不再外调 curl.exe（省掉 8.3 短路径 / ANSI 命令行的坑）。
// 返回 JSON 里 err_no=0 时 pic_str 即验证码文本。
// ============================================================================

#include "newrpa/ocr/ChaojiyingOcr.h"
#include "newrpa/net/HttpClient.h"
#include "newrpa/util/StringUtils.h"
#include "newrpa/crypto/Crypto.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <process.h>   // _getpid

namespace fs = std::filesystem;

namespace newrpa {
namespace ocr {

// ----------------------------------------------------------------------------
// 凭据解析 / 全局单例
// ----------------------------------------------------------------------------

static std::string EnvOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

OcrCred ResolveOcrCred(const OcrCred& fromConfig) {
    OcrCred c = fromConfig;
    c.user     = EnvOr("CJY_USER",     c.user);
    c.pass     = EnvOr("CJY_PASS",     c.pass);
    c.softId   = EnvOr("CJY_SOFTID",   c.softId);
    c.url      = EnvOr("CJY_URL",      c.url);
    c.codeType = EnvOr("CJY_CODETYPE", c.codeType);
    return c;
}

namespace {
OcrCred g_cred;          // 由 SetGlobalOcr 设置（进程级，插件共享）
bool    g_credSet = false;
}

void SetGlobalOcr(const OcrCred& cred) {
    g_cred = cred;
    g_credSet = true;
    spdlog::info("[OCR] global cred: {} (user={} softid={} url={} codetype={})",
                 g_cred.Valid() ? "ready" : "NOT configured",
                 g_cred.user.empty() ? "(env)" : "set",
                 g_cred.softId.empty() ? "(env)" : "set",
                 g_cred.url, g_cred.codeType);
}

bool GlobalOcrReady() { return g_credSet && g_cred.Valid(); }
OcrCred GlobalOcrCred() { return g_cred; }

// ----------------------------------------------------------------------------
// 工具
// ----------------------------------------------------------------------------

namespace {

// 从返回 JSON 里抠字段（响应可能是 GBK，nlohmann 直接 parse 非 UTF-8 会炸，
// 所以用正则抠，中文 err_str 再单独 GBK->UTF8）
bool JsonField(const std::string& resp, const std::string& key,
               std::string& out) {
    std::string pat = "\"" + key + "\"\\s*:\\s*(?:\"([^\"]*)\"|([^,\\}\\n]+))";
    std::regex re(pat);
    std::smatch m;
    if (!std::regex_search(resp, m, re)) return false;
    out = m[1].matched ? m[1].str() : util::Trim(m[2].str());
    return true;
}

std::string NowMs() {
    return std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// MIME -> 文件扩展名（别把 PNG 硬叫成 .jpg 让平台误判）
std::string MimeToExt(const std::string& mime) {
    if (mime.find("png")  != std::string::npos) return ".png";
    if (mime.find("gif")  != std::string::npos) return ".gif";
    if (mime.find("bmp")  != std::string::npos) return ".bmp";
    if (mime.find("webp") != std::string::npos) return ".webp";
    return ".jpg";
}

bool ReadFileBytes(const std::string& path, std::string& bytes,
                   std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        err = "打码图片打不开: " + path;
        return false;
    }
    bytes.assign((std::istreambuf_iterator<char>(f)),
                 std::istreambuf_iterator<char>());
    if (bytes.size() < 64) {
        err = "打码图片只有 " + std::to_string(bytes.size()) + " 字节，太小: " + path;
        return false;
    }
    return true;
}

} // namespace

// ----------------------------------------------------------------------------
// 上传
// ----------------------------------------------------------------------------

bool ChaojiyingOcr::Upload(const std::string& filename,
                           const std::string& bytes,
                           const std::string& codeType,
                           std::string& out, std::string& err) {
    if (!cred_.Valid()) {
        err = "打码账号未配置：在 config.json 顶层加 ocr 段填 user / pass / softid"
              "（或 setx CJY_USER / CJY_PASS / CJY_SOFTID 后重开终端）";
        return false;
    }

    const std::string boundary = "----NewRpaOcr" + NowMs();
    std::string body;

    auto AddField = [&](const std::string& name, const std::string& value) {
        body += "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" +
                value + "\r\n";
    };
    AddField("user",     cred_.user);
    AddField("pass2",    crypto::Md5(cred_.pass));   // 新版接口必须 MD5
    AddField("softid",   cred_.softId);
    AddField("codetype", codeType.empty() ? cred_.codeType : codeType);
    AddField("len_min",  "0");
    // 文件段
    body += "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"userfile\"; filename=\"" +
            filename + "\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n";
    body += bytes;
    body += "\r\n--" + boundary + "--\r\n";

    net::HttpClient http;
    auto resp = http.Post(cred_.url, body,
                          "multipart/form-data; boundary=" + boundary);

    if (resp.status == 0) {
        err = "打码请求失败: " + resp.error;
        return false;
    }
    if (resp.body.empty()) {
        err = "打码接口返回空（HTTP " + std::to_string(resp.status) +
              "，网络不通 / 被墙 / 域名解析失败）";
        return false;
    }

    std::string errNo, picStr, picId, errStr;
    JsonField(resp.body, "err_no", errNo);
    JsonField(resp.body, "pic_id", picId);
    if (errNo != "0") {
        JsonField(resp.body, "err_str", errStr);
        err = "打码失败 err_no=" + errNo + "（" + util::GbkToUtf8(errStr) + "）";
        if (!picId.empty()) err += " pic_id=" + picId;
        return false;
    }
    if (!JsonField(resp.body, "pic_str", picStr) || picStr.empty()) {
        err = "打码返回里没有 pic_str，原始返回: " + resp.body.substr(0, 200);
        return false;
    }

    spdlog::info("[OCR] pic_id={} code={}", picId, picStr);
    out = picStr;
    return true;
}

// ----------------------------------------------------------------------------
// 本地文件
// ----------------------------------------------------------------------------

bool ChaojiyingOcr::OcrFromFile(const std::string& path,
                                const std::string& codeType,
                                std::string& out, std::string& err) {
    std::string bytes;
    if (!ReadFileBytes(path, bytes, err)) return false;
    std::string ext = fs::path(path).extension().string();
    if (ext.empty()) ext = ".png";
    return Upload(fs::path(path).filename().string(), bytes, codeType, out, err);
}

// ----------------------------------------------------------------------------
// 页面内联 base64（data URL 或纯 base64）
// ----------------------------------------------------------------------------

bool ChaojiyingOcr::OcrFromDataUrl(const std::string& src,
                                   const std::string& codeType,
                                   std::string& out, std::string& err) {
    std::string b64, mime = "image/jpeg";
    if (util::StartsWith(src, "data:")) {
        size_t comma = src.find(',');
        if (comma == std::string::npos) {
            err = "data URL 格式不对（没有逗号分隔）";
            return false;
        }
        mime = src.substr(5, src.find(';') - 5);
        b64  = src.substr(comma + 1);
    } else {
        b64 = src;
    }

    auto bin = util::Base64Decode(util::Trim(b64));
    if (bin.size() < 64) {
        err = "图片解码后只有 " + std::to_string(bin.size()) + " 字节，取图失败";
        return false;
    }
    std::string bytes(reinterpret_cast<const char*>(bin.data()), bin.size());

    // 临时目录落盘 -> 上传 -> 删除（平台按扩展名识别格式，不能乱给）
    static std::atomic<unsigned> seq{0};
    fs::path dir;
    try {
        dir = fs::temp_directory_path() / "newrpa_ocr";
        fs::create_directories(dir);
    } catch (const std::exception& e) {
        err = std::string("打码临时目录创建失败: ") + e.what();
        return false;
    }
    const std::string path = (dir / ("ocr_" + NowMs() + "_" +
                                     std::to_string(_getpid()) + "_" +
                                     std::to_string(++seq) +
                                     MimeToExt(mime))).string();

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        err = "写临时图片失败: " + path + "（errno=" + std::to_string(errno) + "）";
        return false;
    }
    bool wrote = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    if (!wrote) {
        std::error_code ec; fs::remove(path, ec);
        err = "写临时图片不完整: " + path;
        return false;
    }
    spdlog::info("[OCR] temp image: {}", path);

    bool ok = Upload(fs::path(path).filename().string(), bytes,
                     codeType, out, err);
    std::error_code ec; fs::remove(path, ec);
    return ok;
}

} // namespace ocr
} // namespace newrpa
