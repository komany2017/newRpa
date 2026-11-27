#pragma once
// ============================================================================
// StateStore / LoginState —— 运行时状态生命周期统一（P6）
//
// 把原先散落的登录态、Cookie 文件、增量日期文件收归一处：
//   * LoginState  : 插件内存中的登录状态（带 raw/epoch，便于诊断）
//   * StateStore  : 状态目录与文件命名规则（路径与旧实现完全一致）
//
// header-only，不新增虚函数、不改变 DLL ABI。
// ============================================================================

#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "newrpa/util/StringUtils.h"

namespace fs = std::filesystem;

namespace newrpa {
namespace arch {

// ----------------------------------------------------------------------------
// 登录态：替代三个插件里各自维护的 bool loggedIn_（避开状态机 enum LoginState）
// ----------------------------------------------------------------------------
class LoginSessionState {
public:
    bool IsLoggedIn() const noexcept { return loggedIn_; }
    explicit operator bool() const noexcept { return loggedIn_; }

    void MarkLoggedIn(std::string raw = "") {
        loggedIn_ = true;
        raw_ = std::move(raw);
        ++epoch_;
    }

    void MarkLoggedOut() {
        loggedIn_ = false;
        raw_.clear();
        ++epoch_;
    }

    const std::string& LastRaw() const noexcept { return raw_; }
    int Epoch() const noexcept { return epoch_; }

private:
    bool        loggedIn_ = false;
    std::string raw_;
    int         epoch_ = 0;
};

// ----------------------------------------------------------------------------
// 状态存储：统一路径生成与增量日期读写
// ----------------------------------------------------------------------------
class StateStore {
public:
    explicit StateStore(fs::path root = fs::absolute("state"))
        : root_(std::move(root)) {}

    const fs::path& Root() const noexcept { return root_; }
    void SetRoot(fs::path root) { root_ = std::move(root); }

    // 与旧 BankCommands 完全一致：state/cookies_<bankName>.json
    fs::path CookieFile(const std::string& bankName) const {
        return root_ / ("cookies_" + bankName + ".json");
    }

    // 与旧 BCBPlugin 完全一致：state/<key>_histran_state.json
    fs::path HisTranFile(const std::string& key) const {
        return root_ / (key + "_histran_state.json");
    }

    bool EnsureRoot() const {
        std::error_code ec;
        fs::create_directories(root_, ec);
        return !ec;
    }

    const std::string& LastError() const noexcept { return lastError_; }

    std::string LoadLastHisEnd(const std::string& key) const {
        std::ifstream f(HisTranFile(key));
        if (!f) return "";
        try {
            nlohmann::json j;
            f >> j;
            return j.value("last_success_end", "");
        } catch (...) { return ""; }
    }

    // 返回 true = 写入成功；异常由调用方按原日志文本处理
    bool SaveLastHisEnd(const std::string& key,
                        const std::string& endYmd) const {
        try {
            fs::path p = HisTranFile(key);
            fs::create_directories(p.parent_path());
            nlohmann::json j;
            std::ifstream f(p);
            if (f) {
                try { f >> j; } catch (...) { j = nlohmann::json::object(); }
            } else {
                j = nlohmann::json::object();
            }
            j["last_success_end"] = endYmd;
            j["updated_at"] = util::FormatTime(std::time(nullptr));
            std::ofstream o(p, std::ios::trunc);
            o << j.dump(2);
            return true;
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return false;
        }
    }

private:
    fs::path    root_;
    mutable std::string lastError_;
};

} // namespace arch
} // namespace newrpa
