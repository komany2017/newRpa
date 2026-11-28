#pragma once
// ============================================================================
// ConfigDb — 结构化加密配置存储（sqlite3）
//
// 设计：config.json 的每个字段拆到 sqlite3 表里，而不是把整个 JSON 加密成 BLOB。
// 敏感字段（银行 password/certPassword、ocr.user/pass）在库内保持 "enc:v1:" 密文，
// 由 LoadConfig 用 crypto.aesKey 解密（字段级加密）。
//
// 库密码（NEWRPA_DB_PASS > 默认 1qazxsw2A）用于打开校验：auth 表存
// PBKDF2-HMAC-SHA256(salt, password) 的 verifier，密码不符拒绝读取。
//
// schema：
//   auth(salt TEXT, verifier TEXT)                  库密码校验
//   meta(k TEXT PRIMARY KEY, v TEXT, kind TEXT)     顶层标量字段
//   list_val(section TEXT, idx INTEGER, v TEXT, kind TEXT)   顶层数组字段
//   obj_val(section TEXT, k TEXT, v TEXT, kind TEXT)         对象字段(crypto/ocr/chromePrefs/httpTasks)
//   banks(id INTEGER, k TEXT, v TEXT, kind TEXT)             银行字段(EAV)
//
// kind: str / num / bool / json —— 保证 JSON 类型无损往返。
// ============================================================================
#include "newrpa/NewRpaExport.h"
#include <string>

namespace newrpa {
namespace util {

NEWRPA_API std::string ConfigDbPassword();

NEWRPA_API bool ConfigDbRead(const std::string& dbPath, std::string& jsonOut,
                             std::string& errOut);

NEWRPA_API bool ConfigDbWrite(const std::string& dbPath, const std::string& jsonText,
                              std::string& errOut);

NEWRPA_API bool ConfigDbCheckPassword(const std::string& dbPath, std::string& errOut);

} // namespace util
} // namespace newrpa
