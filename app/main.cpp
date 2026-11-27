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
//   newrpa ocr --file a.png [--codetype 1004]   # 超级鹰打码自测
// ============================================================================

#include "newrpa/cdp/CDPDomains.h"
#include "newrpa/chrome/ChromeBrowser.h"
#include "newrpa/net/HttpSession.h"
#include "newrpa/ocr/ChaojiyingOcr.h"
#include "newrpa/plugin/IBankPlugin.h"
#include "newrpa/util/Config.h"
#include "newrpa/util/LogSetup.h"
#include "commands/BankCommands.h"
#include "newrpa/arch/Context.h"
#include "newrpa/util/StringUtils.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static void PrintUsage() {
    std::cout << R"(
NewRpa-Next — C++17 + CDP + Chrome 银行 RPA

Usage:
  newrpa.exe init
  newrpa.exe chrome [--port PORT] [--headless] [--profile DIR]
  newrpa.exe list
  newrpa.exe run --bank BANK_NAME --action ACTION [options]
  newrpa.exe ocr --file IMG.png [--codetype 1004]   超级鹰打码自测（公共凭据）

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
  sniff         抓包: 真实登录后记录所有 XHR/Fetch 请求与 Cookie 到 sniff_result.json
  http-direct   混合模式: CDP 登录 + 纯 HTTP 直连下载数据 (无浏览器点击取数)
  http-session  通用模式: RPA 登录 → 抓 Cookie 持久化 → 带 Cookie 按 httpTasks 取数
                (配合 --reuse-cookie 可直接复用 Cookie，跳过登录)

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
  --reuse-cookie      http-session 时跳过登录，复用 state/cookies_<BANK>.json
)";
}

// 简易命令行解析
struct Args {
    std::string cmd; // run / list / init / chrome / ocr
    std::string bank;
    std::string action;
    std::string config = "./config.json";
    std::string begin, end;
    std::string amount, nums, paccno, no;
    std::string file, codeType; // ocr: 图片路径 / codetype
    int chromePort = 0;
    bool noLaunch = false;
    bool headless = false;
    bool reuseCookie = false;
    std::string profileDir;
    std::string logFile; // --log-file，空表示用默认路径

    static Args Parse(int argc, char** argv) {
        Args a;
        for (int i = 1; i < argc; ++i) {
            std::string s = argv[i];
            auto next = [&](std::string& dst) {
                if (i + 1 < argc) {
                    dst = argv[++i];
                    return true;
                }
                return false;
            };
            if (s == "init") {
                a.cmd = "init";
            } else if (s == "list") {
                a.cmd = "list";
            } else if (s == "chrome") {
                a.cmd = "chrome";
            } else if (s == "run") {
                a.cmd = "run";
            } else if (s == "ocr") {
                a.cmd = "ocr";
            } else if (s == "--bank")
                next(a.bank);
            else if (s == "--file")
                next(a.file);
            else if (s == "--codetype")
                next(a.codeType);
            else if (s == "--action")
                next(a.action);
            else if (s == "--config")
                next(a.config);
            else if (s == "--begin")
                next(a.begin);
            else if (s == "--end")
                next(a.end);
            else if (s == "--amount")
                next(a.amount);
            else if (s == "--nums")
                next(a.nums);
            else if (s == "--paccno")
                next(a.paccno);
            else if (s == "--no")
                next(a.no);
            else if (s == "--chrome-port" || s == "--port") { // --port 是 chrome 子命令的简写
                if (i + 1 < argc) {
                    a.chromePort = std::atoi(argv[++i]);
                }
            } else if (s == "--no-launch") {
                a.noLaunch = true;
            } else if (s == "--headless") {
                a.headless = true;
            } else if (s == "--reuse-cookie") {
                a.reuseCookie = true;
            } else if (s == "--profile")
                next(a.profileDir);
            else if (s == "--log-file")
                next(a.logFile);
            else if (s == "--help" || s == "-h") {
                a.cmd = "help";
            }
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

int MainChrome(const Args& args, const newrpa::util::GlobalConfig& cfg) {
    newrpa::chrome::ChromeLauncher::Options opts;
    if (!args.profileDir.empty())
        opts.userDataDir = args.profileDir;
    else if (!cfg.chromeProfileDir.empty())
        opts.userDataDir = cfg.chromeProfileDir;
    if (args.chromePort)
        opts.port = args.chromePort;
    else if (cfg.chromePort > 0)
        opts.port = cfg.chromePort;
    if (args.headless || cfg.headless) opts.headless = true;
    if (!cfg.chromePath.empty()) opts.executablePath = cfg.chromePath;
    // 与 run 命令一致的启动参数 / prefs / 扩展
    opts.extraArgs = cfg.chromeArgs;
    opts.prefsJson = cfg.chromePrefsJson;
    opts.extensions = cfg.extensionPaths;

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

#include "newrpa/core/xskydes.h"
using namespace DES;
#define DESPAGE 4096 * 4

int MainRun(const Args& args, const newrpa::util::GlobalConfig& cfg) {
    try {
        // std::string des = "482f51f3e779190c";
        // char inbuf[DESPAGE] = { 0 };
        // char outbuf[DESPAGE] = { 0 };
        // memcpy(inbuf, des.c_str(), des.length());
        // int len = DesDecrypt(inbuf, outbuf);

        // 1. 找到目标银行
        const newrpa::plugin::BankConfig* bankCfg = nullptr;
        for (auto& b : cfg.banks) {
            if (b.bankName == args.bank || b.displayName == args.bank) {
                bankCfg = &b;
                break;
            }
        }
        if (!bankCfg) {
            spdlog::error("Bank '{}' not found in config", args.bank);
            spdlog::info("Available banks:");
            for (auto& b : cfg.banks) spdlog::info("  - {} ({})", b.bankName, b.displayName);
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
        spdlog::info("Plugin loaded: {} ({})", plugin->GetId(), plugin->GetDisplayName());

        // 3. 启动 / 连接 Chrome
        std::unique_ptr<newrpa::chrome::ChromeLauncher> launcher;
        std::shared_ptr<newrpa::chrome::ChromeBrowser> browser;
        if (!args.noLaunch) {
            newrpa::chrome::ChromeLauncher::Options opts;
            opts.headless = args.headless || cfg.headless;
            // 银行级覆盖 > 全局：KLB 等需要专属 profile（带安全控件扩展）的银行
            opts.port =
                args.chromePort ? args.chromePort : (bankCfg->chromePort > 0 ? bankCfg->chromePort : cfg.chromePort);
            opts.userDataDir = !bankCfg->chromeProfileDir.empty() ? bankCfg->chromeProfileDir : cfg.chromeProfileDir;
            if (!cfg.chromePath.empty()) opts.executablePath = cfg.chromePath;
            // 从配置注入 Chrome 启动参数与 prefs
            opts.extraArgs = cfg.chromeArgs;
            opts.prefsJson = cfg.chromePrefsJson;
            // 解压版扩展（--load-extension）；Chrome ≥137 的开关由 ChromeLauncher 自动补
            opts.extensions = cfg.extensionPaths;
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
        //    （Chrome 156+ 的 /json/new 在启动瞬间存在竞态，易报 "Chrome /json/new
        //    failed"）
        spdlog::info("Opening page...");
        newrpa::chrome::ChromeTarget target;
        bool foundPage = false;
        // 最多重试 10 次等 page target 出现（CDP ready 后 target 列表可能稍晚填充）
        for (int attempt = 0; attempt < 10 && !foundPage; ++attempt) {
            auto targets = browser->ListTargets();
            // 优先非 about:blank，其次任意 page（含 about:blank）
            for (auto& t : targets) {
                if (t.type == "page" && !t.wsUrl.empty() && t.url != "about:blank") {
                    target = t;
                    foundPage = true;
                    break;
                }
            }
            if (!foundPage) {
                for (auto& t : targets) {
                    if (t.type == "page" && !t.wsUrl.empty()) {
                        target = t;
                        foundPage = true;
                        break;
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
            newrpa::cdp::PageDomain pg(session.get());
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

        // 6. 执行 action —— P1：命令总线分发
        //    前置流程（配置查找 / 插件加载 / Chrome 启动 / CDP 连接）与原实现完全一致。
        spdlog::info("Running action: {}", args.action);

        newrpa::arch::AppContext ctx = newrpa::arch::AppContextBuilder()
                                           .WithAction(args.action)
                                           .WithRange(args.begin, args.end)
                                           .WithPayment(args.amount, args.nums, args.paccno)
                                           .WithNo(args.no)
                                           .WithReuseCookie(args.reuseCookie)
                                           .WithConfigPath(args.config)
                                           .WithBank(*bankCfg)
                                           .WithPlugin(plugin.get())
                                           .WithSession(session)
                                           .WithBrowser(browser)
                                           .WithGlobalConfig(&cfg)
                                           .WithDownloadDir(fs::absolute("downloads").string())
                                           .WithStateDir(fs::absolute("state").string())
                                           .Build();

        auto bus = newrpa::app::MakeBankCommandBus();
        auto result = bus->Execute(args.action, ctx);
        if (!result) {
            if (result.error().code == "command.unknown")
                spdlog::error("Unknown action: {}", args.action);
            else if (!result.error().message.empty())
                spdlog::error("{}", result.error().message);
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

// 超级鹰打码自测：newrpa.exe ocr --file xxx.png [--codetype 1004]
int MainOcr(const Args& args, const newrpa::ocr::OcrCred& cred) {
    if (args.file.empty()) {
        spdlog::error("ocr: 需要 --file 图片路径");
        return 1;
    }
    if (!cred.Valid()) {
        spdlog::error("打码账号未配置: 在 config.json 顶层加 ocr 段填 user / pass / softid，"
                      "或 setx CJY_USER / CJY_PASS / CJY_SOFTID 后重开终端");
        return 1;
    }
    newrpa::ocr::ChaojiyingOcr ocr(cred);
    std::string code, err;
    if (!ocr.OcrFromFile(args.file, args.codeType, code, err)) {
        spdlog::error("[OCR] {}", err);
        return 1;
    }
    std::cout << "code=" << code << std::endl;
    return 0;
}

// ----------------------------------------------------------------------------

int main(int argc, char** argv) {

    Args args = Args::Parse(argc, argv);
    newrpa::util::SetupLogging(args.logFile, argv[0]);

    if (args.cmd.empty() || args.cmd == "help") {
        PrintUsage();
        return args.cmd.empty() ? 0 : 1;
    }

    if (args.cmd == "init") return MainInit(args.config);
    if (args.cmd == "chrome") return MainChrome(args, newrpa::util::LoadConfig(args.config));

    auto cfg = newrpa::util::LoadConfig(args.config);

    // 全局打码凭据（公共，一次设置所有插件共享）：config.json ocr 段为底，
    // 环境变量 CJY_* 优先
    newrpa::ocr::OcrCred oc;
    oc.user = cfg.ocrUser;
    oc.pass = cfg.ocrPass;
    oc.softId = cfg.ocrSoftId;
    if (!cfg.ocrUrl.empty()) oc.url = cfg.ocrUrl;
    if (!cfg.ocrCodeType.empty()) oc.codeType = cfg.ocrCodeType;
    newrpa::ocr::SetGlobalOcr(newrpa::ocr::ResolveOcrCred(oc));

    if (args.cmd == "ocr") return MainOcr(args, newrpa::ocr::GlobalOcrCred());
    if (args.cmd == "list") return MainListPlugins(cfg);
    if (args.cmd == "run") return MainRun(args, cfg);

    spdlog::error("Unknown command: {}", args.cmd);
    return 1;
}
