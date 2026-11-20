// ============================================================================
// StringUtils 实现
// ============================================================================
#include "newrpa/util/StringUtils.h"
#include <spdlog/spdlog.h>
#include <algorithm>

#include <sstream>
#include <iomanip>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace newrpa {
namespace util {

std::string FormatTime(std::time_t t, const std::string& fmt) {
    if (t == 0) t = std::time(nullptr);
    char buf[64]{};
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(buf, sizeof(buf), fmt.c_str(), &tm);
    return buf;
}

std::string TodayStr()     { return DateStr(); }
std::string YesterdayStr() { return DateStr(std::time(nullptr) - 86400); }
std::string DateStr(std::time_t t) { return FormatTime(t, "%Y-%m-%d"); }

std::time_t ParseTime(const std::string& s, const std::string& fmt) {
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, fmt.c_str());
    if (ss.fail()) return 0;
#if defined(_WIN32)
    return _mkgmtime(&tm) - 8*3600; // 近似
#else
    return timegm(&tm);
#endif
}

// ----------------------------------------------------------------------------
// 字符串
// ----------------------------------------------------------------------------

std::string Trim(const std::string& s) {
    return LTrim(RTrim(s));
}
std::string LTrim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}
std::string RTrim(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && std::isspace((unsigned char)s[i-1])) --i;
    return s.substr(0, i);
}

std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            result.push_back(cur);
            cur.clear();
        } else cur.push_back(c);
    }
    result.push_back(cur);
    return result;
}

std::string Join(const std::vector<std::string>& parts,
                 const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string ToUpper(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::toupper);
    return r;
}
std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

bool StartsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool EndsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() &&
           s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool Contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

std::string ReplaceAll(const std::string& s,
                       const std::string& from, const std::string& to) {
    std::string result;
    result.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, from.size(), from) == 0) {
            result += to;
            i += from.size();
        } else {
            result.push_back(s[i]);
            ++i;
        }
    }
    return result;
}

std::string UrlEncode(const std::string& s) {
    std::ostringstream oss;
    for (char c : s) {
        if (std::isalnum((unsigned char)c) ||
            c == '-' || c == '_' || c == '.' || c == '~' ||
            c == ':' || c == '/' || c == '?') {
            oss << c;
        } else {
            oss << '%' << std::uppercase << std::hex
                << std::setw(2) << std::setfill('0')
                << (unsigned)(unsigned char)c;
        }
    }
    return oss.str();
}

// ----------------------------------------------------------------------------
// UTF8 / GBK
// ----------------------------------------------------------------------------

#if defined(_WIN32)
std::string GbkToUtf8(const std::string& gbk) {
    // GBK (CP936) → UTF-16 → UTF-8
    int wlen = MultiByteToWideChar(936, 0,
                                  gbk.data(), (int)gbk.size(),
                                  nullptr, 0);
    if (wlen <= 0) return "";
    std::wstring wbuf(wlen, 0);
    MultiByteToWideChar(936, 0, gbk.data(), (int)gbk.size(),
                       wbuf.data(), wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0,
                                  wbuf.data(), wlen,
                                  nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return "";
    std::string ubuf(ulen, 0);
    WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wlen,
                       ubuf.data(), ulen, nullptr, nullptr);
    return ubuf;
}

std::string Utf8ToGbk(const std::string& utf8) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0,
                                  utf8.data(), (int)utf8.size(),
                                  nullptr, 0);
    if (wlen <= 0) return "";
    std::wstring wbuf(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                       wbuf.data(), wlen);

    int glen = WideCharToMultiByte(936, 0,
                                  wbuf.data(), wlen,
                                  nullptr, 0, nullptr, nullptr);
    if (glen <= 0) return "";
    std::string gbuf(glen, 0);
    WideCharToMultiByte(936, 0, wbuf.data(), wlen,
                       gbuf.data(), glen, nullptr, nullptr);
    return gbuf;
}
#else
std::string GbkToUtf8(const std::string&) { return ""; }
std::string Utf8ToGbk(const std::string&) { return ""; }
#endif

// ----------------------------------------------------------------------------
// Base64 (简单实现)
// ----------------------------------------------------------------------------

static const char kB64Alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    int pad = 0;
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < data.size()) n |= ((uint32_t)data[i+1]) << 8;
        else pad = 1;
        if (i + 2 < data.size()) n |= (uint32_t)data[i+2];
        else pad = 2;

        out.push_back(kB64Alpha[(n >> 18) & 0x3F]);
        out.push_back(kB64Alpha[(n >> 12) & 0x3F]);
        out.push_back(pad < 2 ? kB64Alpha[(n >> 6) & 0x3F] : '=');
        out.push_back(pad < 1 ? kB64Alpha[n & 0x3F]       : '=');
    }
    return out;
}

std::vector<uint8_t> Base64Decode(const std::string& s) {
    auto idx = [](char c) -> uint8_t {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return 0;
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i < s.size(); i += 4) {
        uint32_t n = idx(s[i]) << 18 | idx(s[i+1]) << 12;
        if (s[i+2] != '=') n |= idx(s[i+2]) << 6;
        if (s[i+3] != '=') n |= idx(s[i+3]);
        out.push_back((n >> 16) & 0xFF);
        if (s[i+2] != '=') out.push_back((n >> 8) & 0xFF);
        if (s[i+3] != '=') out.push_back(n & 0xFF);
    }
    return out;
}

} // namespace util
} // namespace newrpa
