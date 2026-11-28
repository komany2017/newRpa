// ============================================================================
// ConfigDb — 结构化配置存储实现（sqlite3）
//
// config.json 的每个字段拆到 sqlite3 表，而非整体加密成 BLOB。
// 敏感字段（银行密码、OCR 凭据）保持 "enc:v1:" 密文（字段级加密，由 LoadConfig 解密）。
// 库密码用 PBKDF2-HMAC-SHA256 派生 verifier 存 auth 表，打开时校验。
// ============================================================================

#include "newrpa/util/ConfigDb.h"

#include <botan/mac.h>
#include <botan/hex.h>
#include <botan/auto_rng.h>

#include <nlohmann/json.hpp>

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <vector>

namespace newrpa {
namespace util {

namespace {

using json = nlohmann::json;

constexpr char kDefaultPassword[] = "1qazxsw2A";
constexpr int  kIterations = 60000;

struct DbCloser { void operator()(sqlite3* db) const { if (db) sqlite3_close(db); } };
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;

// PBKDF2-HMAC-SHA256 → raw bytes (RFC 2898)
std::vector<uint8_t> Pbkdf2(const std::string& password,
                            const std::vector<uint8_t>& salt, int iter, size_t dkLen) {
    auto hmac = Botan::MessageAuthenticationCode::create("HMAC(SHA-256)");
    if (!hmac) return {};
    const size_t hlen = 32;
    const size_t blocks = (dkLen + hlen - 1) / hlen;
    std::vector<uint8_t> out;
    const auto* pw = reinterpret_cast<const uint8_t*>(password.data());
    for (size_t i = 1; i <= blocks; ++i) {
        hmac->set_key(pw, password.size());
        hmac->update(salt.data(), salt.size());
        const uint8_t be[4] = {uint8_t(i >> 24), uint8_t(i >> 16), uint8_t(i >> 8), uint8_t(i)};
        hmac->update(be, 4);
        auto u = hmac->final();
        auto t = u;
        for (int j = 1; j < iter; ++j) {
            hmac->set_key(pw, password.size());
            hmac->update(u.data(), u.size());
            u = hmac->final();
            for (size_t k = 0; k < t.size(); ++k) t[k] ^= u[k];
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(dkLen);
    return out;
}

bool Exec(sqlite3* db, const char* sql, std::string& err) {
    char* e = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        err = e ? e : "sqlite exec failed";
        sqlite3_free(e);
        return false;
    }
    return true;
}

bool EnsureSchema(sqlite3* db, std::string& err) {
    static const char* kSql[] = {
        "CREATE TABLE IF NOT EXISTS auth(salt TEXT PRIMARY KEY, verifier TEXT NOT NULL);",
        "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT NOT NULL, kind TEXT NOT NULL);",
        "CREATE TABLE IF NOT EXISTS list_val(section TEXT NOT NULL, idx INTEGER NOT NULL, "
        "v TEXT NOT NULL, kind TEXT NOT NULL, PRIMARY KEY(section,idx));",
        "CREATE TABLE IF NOT EXISTS obj_val(section TEXT NOT NULL, k TEXT NOT NULL, "
        "v TEXT NOT NULL, kind TEXT NOT NULL, PRIMARY KEY(section,k));",
        "CREATE TABLE IF NOT EXISTS banks(id INTEGER NOT NULL, k TEXT NOT NULL, "
        "v TEXT NOT NULL, kind TEXT NOT NULL, PRIMARY KEY(id,k));",
    };
    for (auto* sql : kSql)
        if (!Exec(db, sql, err)) return false;
    return true;
}

// JSON 值 → (文本, kind)
std::pair<std::string, std::string> EncodeValue(const json& v) {
    if (v.is_string())  return {v.get<std::string>(), "str"};
    if (v.is_boolean()) return {v.get<bool>() ? "true" : "false", "bool"};
    if (v.is_number())  return {v.dump(), "num"};
    return {v.dump(), "json"};
}

json DecodeValue(const std::string& s, const std::string& kind) {
    if (kind == "str")  return s;
    if (kind == "bool") return (s == "true");
    if (kind == "num")  return json::parse(s, nullptr, false);
    return json::parse(s, nullptr, false);
}

bool BindText(sqlite3_stmt* st, int idx, const std::string& s) {
    return sqlite3_bind_text(st, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT) == SQLITE_OK;
}

// 核对库密码。auth 为空（首次/迁移前）时 hasAuth=false 并返回 true。
bool VerifyPassword(sqlite3* db, const std::string& password, bool& hasAuth, std::string& err) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT salt,verifier FROM auth LIMIT 1;", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db); return false;
    }
    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { sqlite3_finalize(st); hasAuth = false; return true; }
    if (rc != SQLITE_ROW) { err = sqlite3_errmsg(db); sqlite3_finalize(st); return false; }
    const std::string salt((const char*)sqlite3_column_text(st, 0));
    const std::string verifier((const char*)sqlite3_column_text(st, 1));
    sqlite3_finalize(st);
    hasAuth = true;
    std::vector<uint8_t> saltBytes(salt.begin(), salt.end());
    const auto derived = Pbkdf2(password, saltBytes, kIterations, 32);
    if (derived.empty()) { err = "key derivation failed"; return false; }
    return Botan::hex_encode(derived) == verifier;
}

bool WriteAuth(sqlite3* db, const std::string& password, std::string& err) {
    Botan::AutoSeeded_RNG rng;
    auto saltVec = rng.random_vec<Botan::secure_vector<uint8_t>>(16);
    std::vector<uint8_t> salt(saltVec.begin(), saltVec.end());
    const auto derived = Pbkdf2(password, salt, kIterations, 32);
    if (derived.empty()) { err = "key derivation failed"; return false; }
    Exec(db, "DELETE FROM auth;", err);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO auth(salt,verifier) VALUES(?1,?2);", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db); return false;
    }
    const std::string saltStr(salt.begin(), salt.end());
    const std::string verifier = Botan::hex_encode(derived);
    BindText(st, 1, saltStr); BindText(st, 2, verifier);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db); return false; }
    return true;
}

} // namespace

std::string ConfigDbPassword() {
    const char* env = std::getenv("NEWRPA_DB_PASS");
    return (env && *env) ? std::string(env) : std::string(kDefaultPassword);
}

bool ConfigDbCheckPassword(const std::string& dbPath, std::string& err) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.c_str(), &raw) != SQLITE_OK) {
        err = raw ? sqlite3_errmsg(raw) : "open failed";
        if (raw) sqlite3_close(raw);
        return false;
    }
    DbPtr db(raw);
    if (!EnsureSchema(db.get(), err)) return false;
    bool hasAuth = false;
    return VerifyPassword(db.get(), ConfigDbPassword(), hasAuth, err);
}

bool ConfigDbRead(const std::string& dbPath, std::string& jsonOut, std::string& err) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.c_str(), &raw) != SQLITE_OK) {
        err = raw ? sqlite3_errmsg(raw) : "open failed";
        if (raw) sqlite3_close(raw);
        return false;
    }
    DbPtr db(raw);
    if (!EnsureSchema(db.get(), err)) return false;
    bool hasAuth = false;
    if (!VerifyPassword(db.get(), ConfigDbPassword(), hasAuth, err)) {
        err = "wrong database password";
        return false;
    }
    if (!hasAuth) { err = "database not initialized (run config-import)"; return false; }

    json root = json::object();
    sqlite3_stmt* st = nullptr;

    // meta 标量
    if (sqlite3_prepare_v2(db.get(), "SELECT k,v,kind FROM meta;", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const std::string k((const char*)sqlite3_column_text(st, 0));
            const std::string v((const char*)sqlite3_column_text(st, 1));
            const std::string kind((const char*)sqlite3_column_text(st, 2));
            root[k] = DecodeValue(v, kind);
        }
        sqlite3_finalize(st);
    }

    // list_val 数组
    if (sqlite3_prepare_v2(db.get(), "SELECT section,idx,v,kind FROM list_val ORDER BY section,idx;",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const std::string sec((const char*)sqlite3_column_text(st, 0));
            const std::string v((const char*)sqlite3_column_text(st, 2));
            const std::string kind((const char*)sqlite3_column_text(st, 3));
            if (!root.contains(sec) || !root[sec].is_array()) root[sec] = json::array();
            root[sec].push_back(DecodeValue(v, kind));
        }
        sqlite3_finalize(st);
    }

    // obj_val：crypto / ocr / chromePrefs 为对象；httpTasks 为对象数组
    if (sqlite3_prepare_v2(db.get(), "SELECT section,k,v,kind FROM obj_val ORDER BY section,rowid;",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const std::string sec((const char*)sqlite3_column_text(st, 0));
            const std::string k((const char*)sqlite3_column_text(st, 1));
            const std::string v((const char*)sqlite3_column_text(st, 2));
            const std::string kind((const char*)sqlite3_column_text(st, 3));
            if (sec == "httpTasks") {
                if (!root.contains(sec) || !root[sec].is_array()) root[sec] = json::array();
                json item = json::parse(v, nullptr, false);
                if (!item.is_discarded()) root[sec].push_back(item);
            } else {
                if (!root.contains(sec) || !root[sec].is_object()) root[sec] = json::object();
                root[sec][k] = DecodeValue(v, kind);
            }
        }
        sqlite3_finalize(st);
    }

    // banks EAV → 按 id 重组
    if (sqlite3_prepare_v2(db.get(), "SELECT id,k,v,kind FROM banks ORDER BY id,k;",
                           -1, &st, nullptr) == SQLITE_OK) {
        json banks = json::array();
        int lastId = -1;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const int id = sqlite3_column_int(st, 0);
            const std::string k((const char*)sqlite3_column_text(st, 1));
            const std::string v((const char*)sqlite3_column_text(st, 2));
            const std::string kind((const char*)sqlite3_column_text(st, 3));
            if (lastId != id) { banks.push_back(json::object()); lastId = id; }
            banks.back()[k] = DecodeValue(v, kind);
        }
        sqlite3_finalize(st);
        root["banks"] = banks;
    }

    jsonOut = root.dump(2);
    return true;
}

bool ConfigDbWrite(const std::string& dbPath, const std::string& jsonText, std::string& err) {
    json root;
    try { root = json::parse(jsonText); }
    catch (const std::exception& e) { err = std::string("invalid JSON: ") + e.what(); return false; }

    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.c_str(), &raw) != SQLITE_OK) {
        err = raw ? sqlite3_errmsg(raw) : "open failed";
        if (raw) sqlite3_close(raw);
        return false;
    }
    DbPtr db(raw);
    if (!EnsureSchema(db.get(), err)) return false;

    bool hasAuth = false;
    if (!VerifyPassword(db.get(), ConfigDbPassword(), hasAuth, err)) {
        err = "wrong database password";
        return false;
    }
    if (!hasAuth && !WriteAuth(db.get(), ConfigDbPassword(), err)) return false;

    for (const char* sql : {"DELETE FROM meta;", "DELETE FROM list_val;",
                            "DELETE FROM obj_val;", "DELETE FROM banks;"})
        if (!Exec(db.get(), sql, err)) return false;

    // meta：顶层标量（对象/数组分流到其它表）
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string& k = it.key();
        const json& v = it.value();
        if (v.is_object() || v.is_array()) continue;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.get(), "INSERT INTO meta(k,v,kind) VALUES(?1,?2,?3);",
                               -1, &st, nullptr) != SQLITE_OK) { err = sqlite3_errmsg(db.get()); return false; }
        auto [text, kind] = EncodeValue(v);
        BindText(st, 1, k); BindText(st, 2, text); BindText(st, 3, kind);
        const int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db.get()); return false; }
    }

    // list_val：顶层数组（含 _comment）；httpTasks 是对象数组，走 obj_val
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().is_array() || it.key() == "httpTasks") continue;
        const std::string& sec = it.key();
        int idx = 0;
        for (const auto& item : it.value()) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db.get(), "INSERT INTO list_val(section,idx,v,kind) VALUES(?1,?2,?3,?4);",
                                   -1, &st, nullptr) != SQLITE_OK) { err = sqlite3_errmsg(db.get()); return false; }
            auto [text, kind] = EncodeValue(item);
            BindText(st, 1, sec);
            sqlite3_bind_int(st, 2, idx++);
            BindText(st, 3, text); BindText(st, 4, kind);
            const int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db.get()); return false; }
        }
    }

    // obj_val：crypto / ocr / chromePrefs 对象；httpTasks 数组
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string& sec = it.key();
        const json& v = it.value();
        if (sec == "httpTasks" && v.is_array()) {
            for (const auto& item : v) {
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db.get(), "INSERT INTO obj_val(section,k,v,kind) VALUES(?1,?2,?3,?4);",
                                       -1, &st, nullptr) != SQLITE_OK) { err = sqlite3_errmsg(db.get()); return false; }
                BindText(st, 1, sec);
                const std::string k = item.value("name", "task");
                BindText(st, 2, k); BindText(st, 3, item.dump()); BindText(st, 4, "json");
                const int rc = sqlite3_step(st);
                sqlite3_finalize(st);
                if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db.get()); return false; }
            }
        } else if (v.is_object()) {
            for (auto oit = v.begin(); oit != v.end(); ++oit) {
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db.get(), "INSERT INTO obj_val(section,k,v,kind) VALUES(?1,?2,?3,?4);",
                                       -1, &st, nullptr) != SQLITE_OK) { err = sqlite3_errmsg(db.get()); return false; }
                auto [text, kind] = EncodeValue(oit.value());
                BindText(st, 1, sec); BindText(st, 2, oit.key());
                BindText(st, 3, text); BindText(st, 4, kind);
                const int rc = sqlite3_step(st);
                sqlite3_finalize(st);
                if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db.get()); return false; }
            }
        }
    }

    // banks EAV
    if (root.contains("banks") && root["banks"].is_array()) {
        int id = 0;
        for (const auto& bank : root["banks"]) {
            if (!bank.is_object()) continue;
            for (auto bit = bank.begin(); bit != bank.end(); ++bit) {
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db.get(), "INSERT INTO banks(id,k,v,kind) VALUES(?1,?2,?3,?4);",
                                       -1, &st, nullptr) != SQLITE_OK) { err = sqlite3_errmsg(db.get()); return false; }
                auto [text, kind] = EncodeValue(bit.value());
                sqlite3_bind_int(st, 1, id);
                BindText(st, 2, bit.key()); BindText(st, 3, text); BindText(st, 4, kind);
                const int rc = sqlite3_step(st);
                sqlite3_finalize(st);
                if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db.get()); return false; }
            }
            ++id;
        }
    }
    return true;
}

} // namespace util
} // namespace newrpa
