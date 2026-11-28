// ============================================================================
// ConfigDb — 加密配置存储实现（sqlite3 + AES-256-CBC PKCS7）
//
// PBKDF2-HMAC-SHA256 派生密钥（Botan 静态库未编入 PBKDF2 模块，这里自实现，
// 算法与 RFC 2898 完全一致）：
//   DK = T1, T1 = HMAC(P, salt || INT_32_BE(1))，后续 Ti = HMAC(P, T(i-1) || INT_BE(i))
//   iter = 60000，dkLen = 32
// ============================================================================

#include "newrpa/util/ConfigDb.h"

#include <botan/mac.h>
#include <botan/base64.h>
#include <botan/auto_rng.h>
#include <botan/cipher_mode.h>

#include <sqlite3.h>

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace newrpa {
namespace util {

namespace {

constexpr char kDefaultPassword[] = "1qazxsw2A";
constexpr int  kPbdfIterations    = 60000;
constexpr size_t kSaltLen         = 16;
constexpr size_t kIvLen           = 16;
constexpr size_t kKeyLen          = 32;
constexpr char kConfigKey[]       = "config_json";

// PBKDF2-HMAC-SHA256 → 32 字节原始密钥
std::vector<uint8_t> DeriveKey(const std::string& password,
                               const std::vector<uint8_t>& salt, int iterations) {
    auto hmac = Botan::MessageAuthenticationCode::create("HMAC(SHA-256)");
    if (!hmac) return {};

    std::vector<uint8_t> out;
    const uint32_t blocks = 1;   // 32 bytes / SHA-256(32B) = 1 block

    for (uint32_t i = 1; i <= blocks; ++i) {
        hmac->set_key(reinterpret_cast<const uint8_t*>(password.data()), password.size());
        hmac->update(salt.data(), salt.size());
        const uint8_t be[4] = {
            static_cast<uint8_t>((i >> 24) & 0xFF), static_cast<uint8_t>((i >> 16) & 0xFF),
            static_cast<uint8_t>((i >> 8) & 0xFF),  static_cast<uint8_t>(i & 0xFF)};
        hmac->update(be, 4);
        Botan::secure_vector<uint8_t> u = hmac->final();

        Botan::secure_vector<uint8_t> t = u;
        for (int j = 1; j < iterations; ++j) {
            hmac->set_key(reinterpret_cast<const uint8_t*>(password.data()), password.size());
            hmac->update(u.data(), u.size());
            u = hmac->final();
            for (size_t k = 0; k < t.size(); ++k) t[k] ^= u[k];
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(kKeyLen);
    return out;
}

struct DbCloser {
    void operator()(sqlite3* db) const { if (db) sqlite3_close(db); }
};
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;

bool EnsureTable(sqlite3* db, std::string& errOut) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS config(k TEXT PRIMARY KEY, v TEXT NOT NULL);",
        nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        errOut = err ? err : "CREATE TABLE failed";
        sqlite3_free(err);
        return false;
    }
    return true;
}

} // namespace

std::string ConfigDbPassword() {
    const char* env = std::getenv("NEWRPA_DB_PASS");
    return (env && *env) ? std::string(env) : std::string(kDefaultPassword);
}

bool ConfigDbRead(const std::string& dbPath, std::string& jsonOut, std::string& errOut) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.c_str(), &raw) != SQLITE_OK) {
        errOut = raw ? sqlite3_errmsg(raw) : "sqlite3_open failed";
        if (raw) sqlite3_close(raw);
        return false;
    }
    DbPtr db(raw);
    if (!EnsureTable(db.get(), errOut)) return false;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.get(), "SELECT v FROM config WHERE k=?1;", -1, &st, nullptr)
        != SQLITE_OK) {
        errOut = sqlite3_errmsg(db.get());
        return false;
    }
    sqlite3_bind_text(st, 1, kConfigKey, -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        errOut = (rc == SQLITE_DONE) ? "config_json row not found" : sqlite3_errmsg(db.get());
        return false;
    }
    const unsigned char* blob = sqlite3_column_text(st, 0);
    const int blobLen = sqlite3_column_bytes(st, 0);
    const std::string encoded(blob ? reinterpret_cast<const char*>(blob) : "",
                              blob ? static_cast<size_t>(blobLen) : 0);
    sqlite3_finalize(st);

    Botan::secure_vector<uint8_t> data;
    try { data = Botan::base64_decode(encoded); }
    catch (const std::exception& e) { errOut = std::string("base64: ") + e.what(); return false; }
    if (data.size() < kSaltLen + kIvLen + 16) {
        errOut = "cipher blob too short";
        return false;
    }

    const auto key = DeriveKey(ConfigDbPassword(),
                               std::vector<uint8_t>(data.begin(), data.begin() + kSaltLen),
                               kPbdfIterations);
    if (key.empty()) { errOut = "key derivation failed"; return false; }

    auto dec = Botan::Cipher_Mode::create("AES-256/CBC/PKCS7", Botan::Cipher_Dir::Decryption);
    if (!dec) { errOut = "AES-256/CBC/PKCS7 unavailable"; return false; }
    Botan::secure_vector<uint8_t> ct(data.begin() + kSaltLen + kIvLen, data.end());
    try {
        dec->set_key(key.data(), key.size());
        dec->start(data.data() + kSaltLen, kIvLen);
        dec->finish(ct);
    } catch (const std::exception& e) {
        errOut = std::string("decrypt failed (wrong password?): ") + e.what();
        return false;
    }
    jsonOut.assign(ct.begin(), ct.end());
    return true;
}

bool ConfigDbWrite(const std::string& dbPath, const std::string& jsonText,
                   std::string& errOut) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.c_str(), &raw) != SQLITE_OK) {
        errOut = raw ? sqlite3_errmsg(raw) : "sqlite3_open failed";
        if (raw) sqlite3_close(raw);
        return false;
    }
    DbPtr db(raw);
    if (!EnsureTable(db.get(), errOut)) return false;

    Botan::AutoSeeded_RNG rng;
    const auto salt = rng.random_vec<Botan::secure_vector<uint8_t>>(kSaltLen);
    const auto iv   = rng.random_vec<Botan::secure_vector<uint8_t>>(kIvLen);
    const auto key  = DeriveKey(ConfigDbPassword(),
                                std::vector<uint8_t>(salt.begin(), salt.end()),
                                kPbdfIterations);
    if (key.empty()) { errOut = "key derivation failed"; return false; }

    auto enc = Botan::Cipher_Mode::create("AES-256/CBC/PKCS7", Botan::Cipher_Dir::Encryption);
    if (!enc) { errOut = "AES-256/CBC/PKCS7 unavailable"; return false; }
    Botan::secure_vector<uint8_t> pt(jsonText.begin(), jsonText.end());
    try {
        enc->set_key(key.data(), key.size());
        enc->start(iv.data(), iv.size());
        enc->finish(pt);
    } catch (const std::exception& e) {
        errOut = std::string("encrypt failed: ") + e.what();
        return false;
    }

    Botan::secure_vector<uint8_t> blob(salt.begin(), salt.end());
    blob.insert(blob.end(), iv.begin(), iv.end());
    blob.insert(blob.end(), pt.begin(), pt.end());
    const std::string encoded = Botan::base64_encode(blob.data(), blob.size());

    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO config(k,v) VALUES(?1,?2) "
                      "ON CONFLICT(k) DO UPDATE SET v=excluded.v;";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &st, nullptr) != SQLITE_OK) {
        errOut = sqlite3_errmsg(db.get());
        return false;
    }
    sqlite3_bind_text(st, 1, kConfigKey, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, encoded.c_str(), static_cast<int>(encoded.size()), SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { errOut = sqlite3_errmsg(db.get()); return false; }
    return true;
}

} // namespace util
} // namespace newrpa
