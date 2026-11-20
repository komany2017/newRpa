#pragma once

#include "newrpa/NewRpaExport.h"
// ============================================================================
// Crypto — 加密桩，等 OpenSSL 3.x 链接后再填实现
// ============================================================================
#include <string>

namespace newrpa {
namespace crypto {

NEWRPA_API std::string Md5(const std::string& s);
NEWRPA_API std::string Sha256(const std::string& s);

// IV 长度 16 字节的话用 CBC，空 IV 就用 ECB
NEWRPA_API std::string AesEncrypt(const std::string& plain,
                                   const std::string& key,
                                   const std::string& iv = "");
NEWRPA_API std::string AesDecrypt(const std::string& cipher,
                                   const std::string& key,
                                   const std::string& iv = "");

} // namespace crypto
} // namespace newrpa
