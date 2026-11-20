#pragma once
// ============================================================================
// StringUtils — 字符串/时间/编码通用工具
// ============================================================================
#include <string>
#include <vector>
#include <chrono>
#include <ctime>

namespace newrpa {
namespace util {

// 时间格式化
std::string FormatTime(std::time_t t,
                        const std::string& fmt = "%Y-%m-%d %H:%M:%S");
std::string TodayStr();          // "2026-11-20"
std::string YesterdayStr();
std::string DateStr(std::time_t t = 0);
std::time_t ParseTime(const std::string& s,
                       const std::string& fmt = "%Y-%m-%d %H:%M:%S");

// 字符串
std::string Trim(const std::string& s);
std::string LTrim(const std::string& s);
std::string RTrim(const std::string& s);
std::vector<std::string> Split(const std::string& s, char delim);
std::string Join(const std::vector<std::string>& parts,
                 const std::string& sep);
std::string ToUpper(const std::string& s);
std::string ToLower(const std::string& s);
bool        StartsWith(const std::string& s, const std::string& prefix);
bool        EndsWith(const std::string& s, const std::string& suffix);
bool        Contains(const std::string& hay, const std::string& needle);
std::string ReplaceAll(const std::string& s,
                       const std::string& from, const std::string& to);
std::string UrlEncode(const std::string& s);

// UTF8 / GBK 转换（Windows 上用 MultiByteToWideChar / WideCharToMultiByte）
std::string GbkToUtf8(const std::string& gbk);
std::string Utf8ToGbk(const std::string& utf8);

// Base64
std::string Base64Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> Base64Decode(const std::string& s);

} // namespace util
} // namespace newrpa
