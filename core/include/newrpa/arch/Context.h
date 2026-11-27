#pragma once
// ============================================================================
// AppContext — 一次 RPA 运行的统一上下文
//
// 把当前散落在 MainRun 局部变量、插件成员、全局 OCR 里的运行时依赖
// 收拢成一个可传递对象，命令/步骤/策略都只认这一个对象。
//
// header-only。仅依赖现有公开头文件，不改变 DLL ABI。
// ============================================================================

#include "newrpa/arch/StateStore.h"
#include "newrpa/plugin/IBankPlugin.h"
#include "newrpa/cdp/CDPSession.h"
#include "newrpa/chrome/ChromeBrowser.h"
#include "newrpa/util/Config.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace newrpa {
namespace arch {

struct AppContext {
    // --- 银行与配置 ---
    plugin::BankConfig bank;

    // --- 浏览器/CDP 运行时 ---
    std::shared_ptr<cdp::CDPSession>       session;
    std::shared_ptr<chrome::ChromeBrowser> browser;

    // --- 领域对象 ---
    // 由主程序加载插件后注入；CommandBus 用它做能力检查和统一登录
    plugin::IBankPlugin* bankPlugin = nullptr;
    // P5：IBankPlugin 已是 Result<T> 唯一接口，不再需要并存适配层

    // --- 路径 ---
    std::string downloadDir = "downloads"; // 下游回单落盘根目录
    std::string stateDir    = "state";     // Cookie / 增量日期等持久化
    std::string configPath  = "config.json";
    // P6：状态目录的单一入口，Cookie / 增量日期都由它生成路径
    StateStore  state;

    // --- 运行参数 ---
    int retryCount  = 3;      // 可重试错误的默认次数
    int retryDelayMs = 2000;  // 每次重试间隔
    int stepTimeoutMs = 20000;// 单步超时（回单逐条下载实测需要 20s）

    // --- 运行请求（来自 CLI，P1 阶段由命令读取，行为与原 main.cpp 一致） ---
    std::string action;
    std::string begin, end;          // histran / slip / all 的时间段
    std::string amount, nums, paccno;// payment
    std::string no;                  // approval / query
    bool        reuseCookie = false; // http-session 复用 Cookie

    // --- 全局配置（sniff / http-session 等需要读取 cfg 的命令用） ---
    const util::GlobalConfig* globalConfig = nullptr;

    // --- 输出 ---
    std::vector<std::string> logs;   // 结构化日志，最后统一落盘
    std::vector<std::string> files;  // 本次运行产出的文件

    void Log(std::string line) { logs.push_back(std::move(line)); }
};

// ----------------------------------------------------------------------------
// Builder — 避免 20 个参数的构造函数，并集中做校验
// ----------------------------------------------------------------------------
class AppContextBuilder {
public:
    AppContextBuilder& WithBank(plugin::BankConfig cfg) {
        ctx_.bank = std::move(cfg);
        return *this;
    }
    AppContextBuilder& WithSession(std::shared_ptr<cdp::CDPSession> s) {
        ctx_.session = std::move(s);
        return *this;
    }
    AppContextBuilder& WithBrowser(std::shared_ptr<chrome::ChromeBrowser> b) {
        ctx_.browser = std::move(b);
        return *this;
    }
    AppContextBuilder& WithDownloadDir(std::string dir) {
        ctx_.downloadDir = std::move(dir);
        return *this;
    }
    AppContextBuilder& WithStateDir(std::string dir) {
        ctx_.stateDir = dir;
        ctx_.state.SetRoot(std::move(dir));
        return *this;
    }
    AppContextBuilder& WithConfigPath(std::string path) {
        ctx_.configPath = std::move(path);
        return *this;
    }
    AppContextBuilder& WithRetry(int count, int delayMs) {
        ctx_.retryCount = count;
        ctx_.retryDelayMs = delayMs;
        return *this;
    }
    AppContextBuilder& WithStepTimeout(int ms) {
        ctx_.stepTimeoutMs = ms;
        return *this;
    }

    AppContextBuilder& WithPlugin(plugin::IBankPlugin* p) {
        ctx_.bankPlugin = p;
        return *this;
    }

    AppContextBuilder& WithGlobalConfig(const util::GlobalConfig* g) {
        ctx_.globalConfig = g;
        return *this;
    }

    AppContextBuilder& WithAction(std::string action) {
        ctx_.action = std::move(action);
        return *this;
    }

    AppContextBuilder& WithRange(std::string begin, std::string end) {
        ctx_.begin = std::move(begin);
        ctx_.end = std::move(end);
        return *this;
    }

    AppContextBuilder& WithPayment(std::string amount, std::string nums,
                                   std::string paccno) {
        ctx_.amount = std::move(amount);
        ctx_.nums = std::move(nums);
        ctx_.paccno = std::move(paccno);
        return *this;
    }

    AppContextBuilder& WithNo(std::string no) {
        ctx_.no = std::move(no);
        return *this;
    }

    AppContextBuilder& WithReuseCookie(bool reuse) {
        ctx_.reuseCookie = reuse;
        return *this;
    }

    AppContext Build() const {
        AppContext out = ctx_;
        if (out.state.Root().empty())
            out.state.SetRoot(out.stateDir);
        return out;
    }

private:
    AppContext ctx_;
};

} // namespace arch
} // namespace newrpa
