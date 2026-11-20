// ============================================================================
// NewRpa-Next — 主控程序 (CLI)
//
// 用法：
//   newrpa run --bank ICBC --action login
//   newrpa run --bank ICBC --action curbal
//   newrpa run --bank ICBC --action curtran
//   newrpa run --bank ICBC --action histran --begin 2026-11-01 --end 2026-11-20
//   newrpa run --bank ICBC --action slip --begin 2026-11-01 --end 2026-11-20
//   newrpa list                      # 列出已加载插件
//   newrpa init                      # 生成默认 config.json
//   newrpa chrome                    # 单独启动 Chrome + 调试端口
// ============================================================================

#include "newrpa/chrome/ChromeBrowser.h"
#include "newrpa/cdp/CDPDomains.h"
#include "newrpa/plugin/IBankPlugin.h"
#include "newrpa/util/Config.h"
#include "newrpa/util/StringUtils.h"
#include "newrpa/util/LogSetup.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

static void PrintUsage() {
    std::cout << R"(
NewRpa-Next — C++17 + CDP + Chrome 银行 RPA

Usage:
  newrpa.exe init
  newrpa.exe chrome [--port PORT] [--headless] [--profile DIR]
  newrpa.exe list
  newrpa.exe run --bank BANK_NAME --action ACTION [options]

Actions:
  all-accounts  所有账号: 历史交易(增量,优先)/当日交易/余额/回单
                 (别名: all-account)
  login         登录网银
  curbal        查询当日余额
  curtran       查询当日交易
  histran       查询历史交易 (需要 --begin --end)
  slip          下载电子回单 (需要 --begin --end)
  payment       发起付款 (需要 --amount --nums --paccno)
  approval      审批 (需要 --no)
  query         查询交易状态 (需要 --no)

Options:
  --config FILE       配置文件路径，默认 ./config.json
  --chrome-port N     Chrome 调试端口，默认从 config 读取或 9222
  --no-launch         不自动启动 Chrome，假设已有 Chrome 实例
  --begin YYYY-MM-DD
  --end   YYYY-MM-DD
  --amount STR
  --nums   STR
  --paccno STR
  --no     STR
  --bank NAME         ICBC / TZB / CZBANK ...
  --action ACTION     见上
  --log-file PATH     日志文件路径，默认 <exe目录>/logs/newrpa.log
)";
}

// 简易命令行解析
struct Args {
    std::string cmd;         // run / list / init / chrome
    std::string bank;
    std::string action;
    std::string config = "./config.json";
    std::string begin, end;
    std::string amount, nums, paccno, no;
    int         chromePort = 0;
    bool        noLaunch   = false;
    bool        headless   = false;
    std::string profileDir;
    std::string logFile;     // --log-file，空表示用默认路径

    static Args Parse(int argc, char** argv) {
        Args a;
        for (int i = 1; i < argc; ++i) {
            std::string s = argv[i];
            auto next = [&](std::string& dst) {
                if (i + 1 < argc) { dst = argv[++i]; return true; }
                return false;
            };
            if (s == "init")       { a.cmd = "init"; }
            else if (s == "list")  { a.cmd = "list"; }
            else if (s == "chrome"){ a.cmd = "chrome"; }
            else if (s == "run")   { a.cmd = "run"; }
            else if (s == "--bank")          next(a.bank);
            else if (s == "--action")        next(a.action);
            else if (s == "--config")        next(a.config);
            else if (s == "--begin")         next(a.begin);
            else if (s == "--end")           next(a.end);
            else if (s == "--amount")        next(a.amount);
            else if (s == "--nums")          next(a.nums);
            else if (s == "--paccno")        next(a.paccno);
            else if (s == "--no")            next(a.no);
            else if (s == "--chrome-port") { if (i + 1 < argc) { a.chromePort = std::atoi(argv[++i]); } }
            else if (s == "--no-launch")     { a.noLaunch = true; }
            else if (s == "--headless")      { a.headless = true; }
            else if (s == "--profile")       next(a.profileDir);
            else if (s == "--log-file")      next(a.logFile);
            else if (s == "--help" || s == "-h") { a.cmd = "help"; }
        }
        return a;
    }
};

// ----------------------------------------------------------------------------

int MainInit(const std::string& cfgPath) {
    if (fs::exists(cfgPath)) {
        spdlog::warn("{} already exists", cfgPath);
        return 0;
    }
    newrpa::util::SaveDefaultConfig(cfgPath);
    spdlog::info("Generated default config: {}", cfgPath);
    spdlog::info("Edit bank credentials in that file, then run again.");
    return 0;
}

int MainChrome(const Args& args) {
    newrpa::chrome::ChromeLauncher::Options opts;
    if (!args.profileDir.empty()) opts.userDataDir = args.profileDir;
    if (args.chromePort) opts.port = args.chromePort;
    if (args.headless)   opts.headless = true;

    newrpa::chrome::ChromeLauncher launcher(opts);
    if (!launcher.Launch()) {
        spdlog::error("Chrome launch failed");
        return 1;
    }
    spdlog::info("Chrome ready on port {}", launcher.GetPort());
    spdlog::info("Press Ctrl+C to exit...");
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
}

int MainListPlugins(const newrpa::util::GlobalConfig& cfg) {
    newrpa::plugin::PluginLoader loader;
    loader.ScanDir(cfg.pluginDir);
    auto ids = loader.ListIds();
    if (ids.empty()) {
        spdlog::warn("No plugins found in {}", cfg.pluginDir);
        return 0;
    }
    spdlog::info("Loaded {} plugin(s):", ids.size());
    for (auto& id : ids) spdlog::info("  - {}", id);
    return 0;
}

int MainRun(const Args& args, const newrpa::util::GlobalConfig& cfg) {
    try {
    // 1. 找到目标银行
    const newrpa::plugin::BankConfig* bankCfg = nullptr;
    for (auto& b : cfg.banks) {
        if (b.bankName == args.bank || b.displayName == args.bank) {
            bankCfg = &b; break;
        }
    }
    if (!bankCfg) {
        spdlog::error("Bank '{}' not found in config", args.bank);
        spdlog::info("Available banks:");
        for (auto& b : cfg.banks)
            spdlog::info("  - {} ({})", b.bankName, b.displayName);
        return 1;
    }

    // 2. 加载插件
    newrpa::plugin::PluginLoader loader;
    loader.ScanDir(cfg.pluginDir);
    auto plugin = loader.Create(bankCfg->bankName);
    if (!plugin) {
        spdlog::error("Plugin '{}' not found. Is DLL compiled?", bankCfg->bankName);
        return 1;
    }
    spdlog::info("Plugin loaded: {} ({})",
                 plugin->GetId(), plugin->GetDisplayName());

    // 3. 启动 / 连接 Chrome
    std::unique_ptr<newrpa::chrome::ChromeLauncher> launcher;
    std::shared_ptr<newrpa::chrome::ChromeBrowser> browser;

    if (!args.noLaunch) {
        newrpa::chrome::ChromeLauncher::Options opts;
        opts.headless  = args.headless || cfg.headless;
        opts.port      = args.chromePort ? args.chromePort : cfg.chromePort;
        opts.userDataDir = cfg.chromeProfileDir;
        if (!cfg.chromePath.empty()) opts.executablePath = cfg.chromePath;
        // 从配置注入 Chrome 启动参数与 prefs
        opts.extraArgs  = cfg.chromeArgs;
        opts.prefsJson  = cfg.chromePrefsJson;
        launcher = std::make_unique<newrpa::chrome::ChromeLauncher>(opts);
        if (!launcher->Launch()) {
            spdlog::error("Chrome launch failed");
            return 1;
        }
        browser = std::make_shared<newrpa::chrome::ChromeBrowser>(launcher->GetPort());
    } else {
        int port = args.chromePort ? args.chromePort : cfg.chromePort;
        browser = std::make_shared<newrpa::chrome::ChromeBrowser>(port);
    }

    // 4. 获取页面 + CDP session
    //    复用 Chrome 启动自带的 about:blank page，避免 PUT /json/new
    //    （Chrome 156+ 的 /json/new 在启动瞬间存在竞态，易报 "Chrome /json/new failed"）
    spdlog::info("Opening page...");
    newrpa::chrome::ChromeTarget target;
    bool foundPage = false;
    // 最多重试 10 次等 page target 出现（CDP ready 后 target 列表可能稍晚填充）
    for (int attempt = 0; attempt < 10 && !foundPage; ++attempt) {
        auto targets = browser->ListTargets();
        // 优先非 about:blank，其次任意 page（含 about:blank）
        for (auto& t : targets) {
            if (t.type == "page" && !t.wsUrl.empty() && t.url != "about:blank") {
                target = t; foundPage = true; break;
            }
        }
        if (!foundPage) {
            for (auto& t : targets) {
                if (t.type == "page" && !t.wsUrl.empty()) {
                    target = t; foundPage = true; break;
                }
            }
        }
        if (!foundPage) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    // 兜底：实在没有 page 才 CreatePage，并捕获异常重试
    if (!foundPage) {
        for (int attempt = 0; attempt < 5 && !foundPage; ++attempt) {
            try {
                target = browser->CreatePage("about:blank");
                foundPage = true;
            } catch (const std::exception& e) {
                spdlog::warn("[Main] CreatePage retry {}: {}", attempt + 1, e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
    if (!foundPage) {
        spdlog::error("No usable page target after retries");
        return 1;
    }
    spdlog::info("Using page: {} ({})", target.id, target.url);

    auto sessionUnique = browser->Connect(target);
    if (!sessionUnique) {
        spdlog::error("CDP session failed");
        return 1;
    }
    std::shared_ptr<newrpa::cdp::CDPSession> session = std::move(sessionUnique);
    spdlog::info("CDP connected: {}", target.wsUrl);

    // 开启常用 CDP domain（Chrome 156+ 部分需要显式 enable）
    try {
        newrpa::cdp::PageDomain   pg(session.get());
        newrpa::cdp::RuntimeDomain rt(session.get());
        newrpa::cdp::NetworkDomain nw(session.get());
        session->CallNoWait("Page.enable");
        session->CallNoWait("Runtime.enable");
        session->CallNoWait("Network.enable");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } catch (...) {}

    // 5. 注入插件依赖
    plugin->SetSession(session, browser);
    plugin->SetConfig(*bankCfg);

    // 6. 执行 action
    spdlog::info("Running action: {}", args.action);

    auto hasCap = [&](uint32_t c) {
        return (plugin->Capabilities() & c) == c;
    };

    if (args.action == "all-accounts" || args.action == "all-account") {
        // 所有账号：历史交易(增量,优先) / 当日交易 / 余额 / 电子回单
        auto r = plugin->Login();
        spdlog::info("[ALL-ACC] Login result: {}", r);
        if (r != "ok" && r != "already logged in") {
            spdlog::error("[ALL-ACC] Login failed, abort"); return 1;
        }
        auto summary = plugin->RunAllAccounts();
        spdlog::info("[ALL-ACC] SUMMARY:\n{}", summary);

    } else if (args.action == "all") {
        // 一次登录跑完整流程：余额 / 当日交易 / 历史交易 / 电子回单
        auto r = plugin->Login();
        spdlog::info("[ALL] Login result: {}", r);
        if (r != "ok" && r != "already logged in") {
            spdlog::error("[ALL] Login failed, abort"); return 1;
        }
        auto bal = plugin->GetCurBal();
        spdlog::info("[ALL] CurBal: {} rows", bal.size());
        auto cur = plugin->GetCurTran();
        spdlog::info("[ALL] CurTran: {} rows", cur.size());
        std::string begin = args.begin.empty() ? "2026-01-01" : args.begin;
        std::string end   = args.end.empty()   ? "2026-11-20" : args.end;
        newrpa::plugin::DateRange range{begin, end};
        auto his = plugin->GetHisTran(range);
        spdlog::info("[ALL] HisTran: {} rows", his.size());
        auto files = plugin->DownBankSlip(range);
        for (auto& f : files) spdlog::info("[ALL] Slip: {}", f);

    } else if (args.action == "login") {
        if (!hasCap(newrpa::plugin::IBankPlugin::CAP_LOGIN)) {
            spdlog::error("Plugin doesn't support login"); return 1;
        }
        auto r = plugin->Login();
        spdlog::info("Login result: {}", r);

    } else if (args.action == "curbal") {
        plugin->Login();
        auto rows = plugin->GetCurBal();
        spdlog::info("CurBal: {} rows", rows.size());
        for (auto& row : rows) {
            for (auto& kv : row)
                spdlog::info("  {}={}", kv.first, kv.second);
        }

    } else if (args.action == "curtran") {
        plugin->Login();
        auto rows = plugin->GetCurTran();
        spdlog::info("CurTran: {} rows", rows.size());
        for (auto& r : rows)
            spdlog::info("  {} | {} | {} | {} | {}",
                         r.transDate, r.direction, r.amount,
                         r.opAccount, r.opAccountName);

    } else if (args.action == "histran") {
        if (args.begin.empty() || args.end.empty()) {
            spdlog::error("--begin and --end required"); return 1;
        }
        plugin->Login();
        newrpa::plugin::DateRange range{args.begin, args.end};
        auto rows = plugin->GetHisTran(range);
        spdlog::info("HisTran: {} rows", rows.size());

    } else if (args.action == "slip") {
        if (args.begin.empty() || args.end.empty()) {
            spdlog::error("--begin and --end required"); return 1;
        }
        plugin->Login();
        newrpa::plugin::DateRange range{args.begin, args.end};
        auto files = plugin->DownBankSlip(range);
        for (auto& f : files) spdlog::info("Slip: {}", f);

    } else if (args.action == "payment") {
        auto r = plugin->Payment(args.amount, args.nums, args.paccno);
        spdlog::info("Payment: {}", r);

    } else if (args.action == "approval") {
        auto r = plugin->Approval(args.no);
        spdlog::info("Approval: {}", r);

    } else if (args.action == "query") {
        auto r = plugin->Query(args.no);
        spdlog::info("Query: {}", r);

    } else {
        spdlog::error("Unknown action: {}", args.action);
        return 1;
    }

    plugin->Close();
    return 0;
    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return 1;
    } catch (...) {
        spdlog::error("Unknown fatal error");
        return 1;
    }
}

// ----------------------------------------------------------------------------

int main(int argc, char** argv) {
    Args args = Args::Parse(argc, argv);
    newrpa::util::SetupLogging(args.logFile, argv[0]);

    if (args.cmd.empty() || args.cmd == "help") {
        PrintUsage();
        return args.cmd.empty() ? 0 : 1;
    }

    if (args.cmd == "init")     return MainInit(args.config);
    if (args.cmd == "chrome")   return MainChrome(args);

    auto cfg = newrpa::util::LoadConfig(args.config);

    if (args.cmd == "list")     return MainListPlugins(cfg);
    if (args.cmd == "run")      return MainRun(args, cfg);

    spdlog::error("Unknown command: {}", args.cmd);
    return 1;
}
