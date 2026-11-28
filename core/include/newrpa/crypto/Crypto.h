#pragma once

#include "newrpa/NewRpaExport.h"
// ============================================================================
// Crypto — 公共加解密组件（Botan 3.x 静态库）
//
// 密钥来源（全部由 config.json 顶层 "crypto" 段配置，不落源码/日志/Git）：
//   crypto.aesKey            AES-256 主密钥（64 位 hex = 32 字节）
//   crypto.rsaPrivateKeyFile RSA 私钥 PEM 文件路径（PKCS#8）
//   crypto.rsaPublicKeyFile  RSA 公钥 PEM 文件路径（X.509 SPKI）
//
// 密码存储格式（"enc:v1:" 前缀）：
//   enc:v1:<base64( randomIV(16B) || AES-256-CBC-PKCS7(plain) )>
//   - IV 每次加密随机生成，拼在密文头部，无需单独存
//   - 明文密码完全兼容：DecryptPassword 对无前缀的值原样放行
//
// RSA 签名：PKCS#1 v1.5 + SHA-256，输出 base64；密钥按配置从文件读取。
// ============================================================================
#include <string>

namespace newrpa {
namespace crypto {

// ---- 摘要 ----
NEWRPA_API std::string Md5(const std::string& s);     // Config.cpp 自实现（超级鹰 pass2）
NEWRPA_API std::string Sha256(const std::string& s);  // Botan SHA-256，返回 64 位 hex

// ---- AES-CBC + PKCS7 原语（Botan）----
// key: 16/24/32 字节原始串（AES-128/192/256）；iv: 16 字节原始串
// 返回 base64(密文)；解密输入 base64(密文)。失败返回空串并记 error 日志。
NEWRPA_API std::string AesCbcPkcs7Encrypt(const std::string& plain,
                                          const std::string& key,
                                          const std::string& iv);
NEWRPA_API std::string AesCbcPkcs7Decrypt(const std::string& cipherB64,
                                          const std::string& key,
                                          const std::string& iv);

// ---- 主密钥管理（来自 config.json 的 crypto.aesKey）----
// 把 64 位 hex 解析为 32 字节原始密钥；失败返回空串。
NEWRPA_API std::string ParseAesKeyHex(const std::string& hex);

// ---- 密码存储级封装（银行登录密码 / 证书密码统一走这里）----
// 判断是否为 "enc:v1:" 密文
NEWRPA_API bool IsEncryptedPassword(const std::string& s);

// 明文 → "enc:v1:..."。aesKeyRaw 为 ParseAesKeyHex 得到的 32 字节原始密钥。
NEWRPA_API std::string EncryptPassword(const std::string& plain,
                                       const std::string& aesKeyRaw);

// "enc:v1:..." → 明文；明文原样返回（兼容手工配置）；
// 解密失败返回空串并记 error 日志。
NEWRPA_API std::string DecryptPassword(const std::string& stored,
                                       const std::string& aesKeyRaw);

// ---- RSA 签名 / 验签（密钥文件路径由 config.json 配置）----
// 生成 2048 位 RSA 密钥对，写入两个 PEM 文件（PKCS#8 私钥 + X.509 SPKI 公钥）
// bits 可指定（>=1024）。返回 true 表示两个文件都已写出。
NEWRPA_API bool RsaGenerateKeyPair(const std::string& privatePemPath,
                                   const std::string& publicPemPath,
                                   size_t bits = 2048);

// 用私钥 PEM 对 data 签名（PKCS1v15/SHA-256），返回 base64 签名；失败返回空串。
NEWRPA_API std::string RsaSign(const std::string& data,
                               const std::string& privatePemPath);

// 用公钥 PEM 验证 base64 签名；数据/签名/公钥任一异常返回 false。
NEWRPA_API bool RsaVerify(const std::string& data,
                          const std::string& signatureB64,
                          const std::string& publicPemPath);

} // namespace crypto
} // namespace newrpa
