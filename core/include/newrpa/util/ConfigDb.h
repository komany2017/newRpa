#pragma once
// ============================================================================
// ConfigDb — 加密配置存储（sqlite3 + AES-256-CBC PKCS7）
//
// 配置文件不再是明文 JSON，而是一个 sqlite3 数据库：
//   table config(k TEXT PRIMARY KEY, v TEXT NOT NULL)
//   k = "config_json"，v = base64( salt(16B) || IV(16B) || 密文 )
//
// 加密密钥由密码经 PBKDF2-HMAC-SHA256（60000 轮）派生为 32 字节。
// 密码来源：环境变量 NEWRPA_DB_PASS 优先，缺省用内置默认密码。
//
// 任何读取失败（文件缺失/密码错误/密文损坏/表缺失）都返回 false，
// 调用方 LoadConfig 据此退回空配置并报错——绝不静默读明文。
// ============================================================================
#include "newrpa/NewRpaExport.h"
#include <string>

namespace newrpa {
namespace util {

// 取当前生效的数据库密码（环境变量 NEWRPA_DB_PASS > 内置默认值）。
// 仅用于诊断显示，调用方不要把返回值写日志。
NEWRPA_API std::string ConfigDbPassword();

// 从加密 sqlite3 读取配置 JSON 文本。成功返回 true。
NEWRPA_API bool ConfigDbRead(const std::string& dbPath, std::string& jsonOut,
                             std::string& errOut);

// 把配置 JSON 文本写入加密 sqlite3（覆盖已有记录）。成功返回 true。
NEWRPA_API bool ConfigDbWrite(const std::string& dbPath, const std::string& jsonText,
                              std::string& errOut);

} // namespace util
} // namespace newrpa
