// ============================================================================
// Crypto — 公共加解密实现（Botan 3.x 静态库）
//
// 密钥来源全部由 config.json 顶层 crypto 段配置：
//   aesKey (hex 32B) → AES-256-CBC PKCS7 密码加解密
//   rsaPrivateKeyFile / rsaPublicKeyFile (PEM) → RSA 签名 / 验签
//
// 模块依赖：aes / cbc / pkcs7 padding / base64 / system rng / sha2_32 / rsa / pkcs8 / x509
// 链接：third_party/botan/lib/libbotan-3.a（见 core/CMakeLists.txt）
// ============================================================================

#include "newrpa/crypto/Crypto.h"

#include <botan/cipher_mode.h>
#include <botan/auto_rng.h>
#include <botan/base64.h>
#include <botan/hex.h>
#include <botan/hash.h>
#include <botan/rsa.h>
#include <botan/pkcs8.h>
#include <botan/x509_key.h>
#include <botan/pubkey.h>
#include <botan/data_src.h>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace newrpa {
namespace crypto {

namespace {

constexpr char kPrefix[]    = "enc:v1:";
constexpr size_t kPrefixLen = 7;
constexpr size_t kIvLen     = 16;
constexpr size_t kKeyLen    = 32;   // AES-256

// PKCS#1 v1.5 + SHA-256（兼容性最好，银行/第三方接口普遍支持）
constexpr char kRsaPad[] = "PKCS1v15(SHA-256)";

std::unique_ptr<Botan::Cipher_Mode> MakeCipher(Botan::Cipher_Dir dir) {
    return Botan::Cipher_Mode::create("AES-256/CBC/PKCS7", dir);
}

std::string Sha256Hex(const std::string& data) {
    auto hash = Botan::HashFunction::create("SHA-256");
    if (!hash) return "";
    hash->update(data);
    return Botan::hex_encode(hash->final());
}

bool WriteTextFile(const std::string& path, const std::string& content) {
    std::error_code ec;
    const fs::path p(path);
    if (!p.parent_path().empty())
        fs::create_directories(p.parent_path(), ec);
    if (ec) {
        spdlog::error("[Crypto] cannot create key dir {}: {}", path, ec.message());
        return false;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        spdlog::error("[Crypto] cannot write key file: {}", path);
        return false;
    }
    out << content << "\n";
    out.close();
    return true;
}

} // namespace

// ----------------------------------------------------------------------------
// 摘要
// ----------------------------------------------------------------------------

std::string Sha256(const std::string& s) {
    return Sha256Hex(s);
}

// ----------------------------------------------------------------------------
// AES-CBC PKCS7 原语
// ----------------------------------------------------------------------------

std::string AesCbcPkcs7Encrypt(const std::string& plain,
                               const std::string& key,
                               const std::string& iv) {
    if (key.size() != kKeyLen) {
        spdlog::error("[Crypto] AesCbcPkcs7Encrypt: key must be 32 bytes, got {}", key.size());
        return "";
    }
    if (iv.size() != kIvLen) {
        spdlog::error("[Crypto] AesCbcPkcs7Encrypt: iv must be 16 bytes, got {}", iv.size());
        return "";
    }
    auto enc = MakeCipher(Botan::Cipher_Dir::Encryption);
    if (!enc) {
        spdlog::error("[Crypto] AES-256/CBC/PKCS7 not available in this Botan build");
        return "";
    }
    Botan::secure_vector<uint8_t> pt(plain.begin(), plain.end());
    enc->set_key(reinterpret_cast<const uint8_t*>(key.data()), key.size());
    enc->start(reinterpret_cast<const uint8_t*>(iv.data()), iv.size());
    try {
        enc->finish(pt);
    } catch (const std::exception& e) {
        spdlog::error("[Crypto] encrypt finish failed: {}", e.what());
        return "";
    }
    return Botan::base64_encode(pt.data(), pt.size());
}

std::string AesCbcPkcs7Decrypt(const std::string& cipherB64,
                               const std::string& key,
                               const std::string& iv) {
    if (key.size() != kKeyLen || iv.size() != kIvLen) {
        spdlog::error("[Crypto] AesCbcPkcs7Decrypt: bad key/iv size");
        return "";
    }
    auto dec = MakeCipher(Botan::Cipher_Dir::Decryption);
    if (!dec) {
        spdlog::error("[Crypto] AES-256/CBC/PKCS7 not available in this Botan build");
        return "";
    }
    Botan::secure_vector<uint8_t> ct;
    try {
        ct = Botan::base64_decode(cipherB64);
    } catch (const std::exception& e) {
        spdlog::error("[Crypto] base64 decode failed: {}", e.what());
        return "";
    }
    if (ct.empty() || ct.size() % 16 != 0) {
        spdlog::error("[Crypto] cipher size invalid: {}", ct.size());
        return "";
    }
    dec->set_key(reinterpret_cast<const uint8_t*>(key.data()), key.size());
    dec->start(reinterpret_cast<const uint8_t*>(iv.data()), iv.size());
    try {
        dec->finish(ct);
    } catch (const std::exception& e) {
        spdlog::error("[Crypto] decrypt finish failed: {}", e.what());
        return "";
    }
    return std::string(ct.begin(), ct.end());
}

// ----------------------------------------------------------------------------
// 主密钥（config.json crypto.aesKey）
// ----------------------------------------------------------------------------

std::string ParseAesKeyHex(const std::string& hex) {
    std::string trimmed;
    for (char c : hex)
        if (!std::isspace(static_cast<unsigned char>(c))) trimmed.push_back(c);
    if (trimmed.size() != kKeyLen * 2) {
        spdlog::error("[Crypto] aesKey must be 64 hex chars, got {}", trimmed.size());
        return "";
    }
    try {
        auto key = Botan::hex_decode(trimmed);
        return std::string(key.begin(), key.end());
    } catch (const std::exception& e) {
        spdlog::error("[Crypto] aesKey hex decode failed: {}", e.what());
        return "";
    }
}

// ----------------------------------------------------------------------------
// 密码存储级封装
// ----------------------------------------------------------------------------

bool IsEncryptedPassword(const std::string& s) {
    return s.size() > kPrefixLen && s.compare(0, kPrefixLen, kPrefix) == 0;
}

std::string EncryptPassword(const std::string& plain, const std::string& aesKeyRaw) {
    if (plain.empty()) return "";
    if (aesKeyRaw.size() != kKeyLen) {
        spdlog::error("[Crypto] EncryptPassword: aesKey must be 32 bytes (config crypto.aesKey)");
        return "";
    }

    Botan::AutoSeeded_RNG rng;
    const Botan::secure_vector<uint8_t> iv =
        rng.random_vec<Botan::secure_vector<uint8_t>>(kIvLen);
    std::string ivStr(reinterpret_cast<const char*>(iv.data()), iv.size());

    const std::string ctB64 = AesCbcPkcs7Encrypt(plain, aesKeyRaw, ivStr);
    if (ctB64.empty()) return "";

    // 密文格式：enc:v1: base64( IV || 原始密文 )
    const auto rawCt = Botan::base64_decode(ctB64);
    Botan::secure_vector<uint8_t> blob = iv;
    blob.insert(blob.end(), rawCt.begin(), rawCt.end());
    return std::string(kPrefix) + Botan::base64_encode(blob.data(), blob.size());
}

std::string DecryptPassword(const std::string& stored, const std::string& aesKeyRaw) {
    if (stored.empty() || !IsEncryptedPassword(stored))
        return stored;   // 明文配置原样放行

    if (aesKeyRaw.size() != kKeyLen) {
        spdlog::error("[Crypto] DecryptPassword: aesKey missing/bad (config crypto.aesKey)");
        return "";
    }

    const std::string b64 = stored.substr(kPrefixLen);
    Botan::secure_vector<uint8_t> blob;
    try {
        blob = Botan::base64_decode(b64);
    } catch (const std::exception& e) {
        spdlog::error("[Crypto] DecryptPassword: base64 decode failed: {}", e.what());
        return "";
    }
    if (blob.size() <= kIvLen || (blob.size() - kIvLen) % 16 != 0) {
        spdlog::error("[Crypto] DecryptPassword: blob size invalid: {}", blob.size());
        return "";
    }

    std::string ivStr(reinterpret_cast<const char*>(blob.data()), kIvLen);
    const std::string ctB64 = Botan::base64_encode(blob.data() + kIvLen, blob.size() - kIvLen);
    return AesCbcPkcs7Decrypt(ctB64, aesKeyRaw, ivStr);
}

// ----------------------------------------------------------------------------
// RSA 签名 / 验签 / 密钥生成
// ----------------------------------------------------------------------------

bool RsaGenerateKeyPair(const std::string& privatePemPath,
                        const std::string& publicPemPath,
                        size_t bits) {
    if (bits < 1024) {
        spdlog::error("[Crypto] RSA key bits too small: {}", bits);
        return false;
    }
    try {
        Botan::AutoSeeded_RNG rng;
        Botan::RSA_PrivateKey key(rng, bits);
        const std::string priv = Botan::PKCS8::PEM_encode(key);
        const std::string pub  = Botan::X509::PEM_encode(key);
        if (!WriteTextFile(privatePemPath, priv) || !WriteTextFile(publicPemPath, pub))
            return false;
        spdlog::info("[Crypto] RSA-{} key pair generated: {} / {}",
                     bits, privatePemPath, publicPemPath);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[Crypto] RSA keygen failed: {}", e.what());
        return false;
    }
}

std::string RsaSign(const std::string& data, const std::string& privatePemPath) {
    try {
        Botan::DataSource_Stream in(privatePemPath, false);
        auto key = Botan::PKCS8::load_key(in);
        if (!key) {
            spdlog::error("[Crypto] RSA private key load failed: {}", privatePemPath);
            return "";
        }
        Botan::AutoSeeded_RNG rng;
        Botan::PK_Signer signer(*key, rng, kRsaPad);
        const auto sig = signer.sign_message(
            reinterpret_cast<const uint8_t*>(data.data()), data.size(), rng);
        return Botan::base64_encode(sig.data(), sig.size());
    } catch (const std::exception& e) {
        spdlog::error("[Crypto] RSA sign failed: {}", e.what());
        return "";
    }
}

bool RsaVerify(const std::string& data,
               const std::string& signatureB64,
               const std::string& publicPemPath) {
    try {
        auto key = Botan::X509::load_key(publicPemPath);
        if (!key) {
            spdlog::error("[Crypto] RSA public key load failed: {}", publicPemPath);
            return false;
        }
        Botan::PK_Verifier verifier(*key, kRsaPad);
        const auto sig = Botan::base64_decode(signatureB64);
        return verifier.verify_message(
            reinterpret_cast<const uint8_t*>(data.data()), data.size(),
            sig.data(), sig.size());
    } catch (const std::exception& e) {
        spdlog::error("[Crypto] RSA verify failed: {}", e.what());
        return false;
    }
}

} // namespace crypto
} // namespace newrpa
