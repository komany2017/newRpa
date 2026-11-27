// ============================================================================
// ChromeLauncher / ChromeBrowser 实现
// ============================================================================

#include "newrpa/chrome/ChromeBrowser.h"
#include "newrpa/cdp/CDPSession.h"

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <sstream>
#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <shlobj.h>
#else
#  include <unistd.h>
#  include <signal.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace newrpa {
namespace chrome {

using cdp::CDPSession;

// ----------------------------------------------------------------------------
// ChromeLauncher
// ----------------------------------------------------------------------------

// 默认 Chrome profile 目录：exe 同级的 profiles/default（持久化，保留登录态）
static std::string DefaultProfileDir() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) {
        fs::path exe(buf);
        return (exe.parent_path() / "profiles" / "default").string();
    }
#endif
    return (fs::temp_directory_path() / "newrpa-profiles" / "default").string();
}

ChromeLauncher::ChromeLauncher(const Options& opts) : opts_(opts) {
    if (opts_.port <= 0) opts_.port = 9222;
    if (opts_.executablePath.empty())
        opts_.executablePath = FindChromeExecutable();
    if (opts_.userDataDir.empty()) {
        // 配置未指定 profile：用 exe 同级 profiles/default 作为持久化默认目录，
        // 保留登录态/cookies（不用 %TEMP%，避免退出时被清理）
        opts_.userDataDir = DefaultProfileDir();
        fs::create_directories(opts_.userDataDir);
    }
    wsPrefix_ = "ws://127.0.0.1:" + std::to_string(opts_.port) + "/devtools";
}

ChromeLauncher::~ChromeLauncher() {
    if (!opts_.keepRunning) Shutdown();
}

// 按点号路径设置 JSON 值：如 "download.prompt_for_download" -> {"download":{"prompt_for_download":...}}
static void SetJsonByPath(json& root, const std::string& dotPath, const json& value) {
    std::vector<std::string> parts;
    std::string cur;
    std::stringstream ss(dotPath);
    while (std::getline(ss, cur, '.')) {
        if (!cur.empty()) parts.push_back(cur);
    }
    if (parts.empty()) return;
    json* node = &root;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!node->contains(parts[i]) || !(*node)[parts[i]].is_object())
            (*node)[parts[i]] = json::object();
        node = &(*node)[parts[i]];
    }
    (*node)[parts.back()] = value;
}

// 把配置里的 chromePrefs（点号路径 JSON）合并写入 <userDataDir>/Default/Preferences
// 等价 Selenium 的 add_experimental_option("prefs", {...})
static void ApplyChromePrefs(const std::string& userDataDir, const std::string& prefsJson) {
    if (prefsJson.empty()) return;
    try {
        fs::path dir = fs::path(userDataDir) / "Default";
        fs::create_directories(dir);
        fs::path prefFile = dir / "Preferences";
        json prefs = json::object();
        std::ifstream in(prefFile);
        if (in) {
            try { in >> prefs; } catch (...) { prefs = json::object(); }
        }
        json patch = json::parse(prefsJson);
        if (!patch.is_object()) { spdlog::warn("[Chrome] chromePrefs is not an object"); return; }
        int n = 0;
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            SetJsonByPath(prefs, it.key(), it.value());
            ++n;
        }
        std::ofstream out(prefFile);
        out << prefs.dump();
        spdlog::info("[Chrome] Wrote {} prefs to {}", n, prefFile.string());
    } catch (const std::exception& e) {
        spdlog::warn("[Chrome] Failed to write prefs: {}", e.what());
    }
}

// ----------------------------------------------------------------------------
// 启动辅助
// ----------------------------------------------------------------------------

// 端口上是否已经有可用的 CDP 端点。
// 有 = 要么上次运行残留的 Chrome 还活着，要么用户按 launch.json 手工起好了
// 浏览器（--no-launch 场景）。这两种情况都不该再拉一个新进程去抢同一个 profile。
static bool CdpPortReady(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1);
    auto resp = cli.Get("/json/version");
    return resp && resp->status == 200;
}

#if defined(_WIN32)
// UTF-8 -> UTF-16。CreateProcessA 走 ANSI 代码页，中文用户名/路径会被按 GBK
// 重新解释成乱码路径，Chrome 起来就找不到 profile。
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
    return w;
}
#endif

#if defined(_WIN32)
// 当前进程是否跑在低完整性（Low Mandatory Level）令牌下。
// 沙箱/受限容器里的进程就是这种令牌：Chrome 在这种令牌下 spawn 自己的
// crash handler 后 OpenProcess 会被拒（crashpad_client_win.cc 0x5），
// 于是走 ImmediateCrash 自杀，退出码 0x80000003 或 0xFFFF7001。
// 这个失败跟命令行参数、profile、开关全都没关系，任何参数都救不回来。
static bool IsLowIntegrityProcess() {
    HANDLE hTok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok))
        return false;
    DWORD len = 0;
    GetTokenInformation(hTok, TokenIntegrityLevel, nullptr, 0, &len);
    bool low = false;
    if (len > 0) {
        std::vector<BYTE> buf(len);
        if (GetTokenInformation(hTok, TokenIntegrityLevel, buf.data(), len, &len)) {
            auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
            if (label && label->Label.Sid) {
                DWORD cnt = *GetSidSubAuthorityCount(label->Label.Sid);
                if (cnt > 0) {
                    DWORD rid = *GetSidSubAuthority(label->Label.Sid, cnt - 1);
                    low = rid < SECURITY_MANDATORY_MEDIUM_RID;
                }
            }
        }
    }
    CloseHandle(hTok);
    return low;
}
#endif

bool ChromeLauncher::Launch() {
    if (launched_) return true;

    if (opts_.executablePath.empty()) {
        spdlog::error("[Chrome] No Chrome/Edge executable found");
        return false;
    }

    // ---- 0) 端口上已有 CDP：直接复用，不再新起进程 ----
    if (CdpPortReady(opts_.port)) {
        spdlog::info("[Chrome] 端口 {} 上已有 CDP 端点，复用现有浏览器（不新建进程）",
                     opts_.port);
        launched_ = true;
        pid_ = 0;          // pid 未知 -> 析构/Shutdown 不会去杀别人的浏览器
        return true;
    }

#if defined(_WIN32)
    // 低完整性令牌下 Chrome 必然秒退（Crashpad 起不来）。早点说清楚，
    // 免得把 0xFFFF7001 误当成参数问题去一遍遍调命令行。
    if (IsLowIntegrityProcess()) {
        spdlog::warn(
            "[Chrome] 当前进程是 Low Mandatory Level（沙箱/受限令牌），"
            "Chrome 在这种令牌下无法启动（Crashpad 起不来 -> 自杀 0xFFFF7001）。"
            "请换普通权限的终端运行，或用 --no-launch 接已在运行的浏览器。");
    }
#endif

    // 启动前写入 Chrome Preferences（必须在 Chrome 进程启动前落盘）
    // NEWRPA_NO_PREFS=1 可跳过（诊断用：区分 prefs 写入导致的启动异常）
    if (std::getenv("NEWRPA_NO_PREFS") == nullptr)
        ApplyChromePrefs(opts_.userDataDir, opts_.prefsJson);

    // 检测是否配置了 --enable-extensions（与硬编码 --disable-extensions 互斥）
    bool enableExt = false;
    for (auto& a : opts_.extraArgs) {
        if (a.rfind("--enable-extensions", 0) == 0) { enableExt = true; break; }
    }

    // ------------------------------------------------------------------
    // 扩展加载：--load-extension / --disable-extensions-except
    // Chrome ≥137（M137）默认忽略 --load-extension，必须同时给
    //   --disable-features=DisableLoadExtensionCommandLineSwitch
    // 否则命令行看似正常、扩展却完全不加载（无任何报错）。
    // 这里只要有扩展要加载，或用户在 chromeArgs 里自己写了 --load-extension，
    // 就自动把该开关补上；用户已自行指定 --disable-features 时做合并追加。
    // ------------------------------------------------------------------
    std::vector<std::string> extDirs = opts_.extensions;
    bool userPassedLoadExt = false;
    for (auto& a : opts_.extraArgs) {
        if (a.rfind("--load-extension", 0) == 0) {
            userPassedLoadExt = true;
            // --load-extension=dir1,dir2
            auto eq = a.find('=');
            if (eq != std::string::npos) {
                std::string v = a.substr(eq + 1);
                size_t p = 0;
                while (p <= v.size()) {
                    auto c = v.find(',', p);
                    std::string one = v.substr(p, c == std::string::npos ? std::string::npos : c - p);
                    if (!one.empty()) extDirs.push_back(one);
                    if (c == std::string::npos) break;
                    p = c + 1;
                }
            }
        }
    }
    const bool wantExt = !extDirs.empty();

    // 拼扩展目录列表
    std::string extList;
    for (auto& d : extDirs) {
        if (!extList.empty()) extList += ",";
        extList += d;
    }

    // ------------------------------------------------------------------
    // 归一化 --disable-features
    // Chrome 对同名开关只取最后一次出现的值，命令行里若有多条
    // --disable-features=...，前面的会被后面的整条覆盖。配置里就同时有
    // RendererCodeIntegrity 和 LocalNetworkAccessChecks 两条，而我们需要
    // 补的 DisableLoadExtensionCommandLineSwitch 一旦拼在前一条上就等于没给。
    // 所以这里把所有值合并成唯一一条（去重），需要加载扩展时再补开关。
    // ------------------------------------------------------------------
    std::vector<std::string> extraArgs = opts_.extraArgs;
    {
        const std::string kPrefix = "--disable-features=";
        std::vector<std::string> merged;          // 去重后的 feature 名
        std::vector<std::string> kept;
        auto addFeature = [&](const std::string& f) {
            if (f.empty()) return;
            if (std::find(merged.begin(), merged.end(), f) == merged.end())
                merged.push_back(f);
        };
        for (const auto& a : extraArgs) {
            if (a.rfind(kPrefix, 0) != 0) { kept.push_back(a); continue; }
            std::string v = a.substr(kPrefix.size());
            size_t p = 0;
            while (p <= v.size()) {
                auto c = v.find(',', p);
                addFeature(v.substr(p, c == std::string::npos ? std::string::npos : c - p));
                if (c == std::string::npos) break;
                p = c + 1;
            }
        }
        if (wantExt) addFeature("DisableLoadExtensionCommandLineSwitch");
        extraArgs = std::move(kept);
        if (!merged.empty()) {
            std::string joined;
            for (const auto& f : merged) {
                if (!joined.empty()) joined += ",";
                joined += f;
            }
            extraArgs.push_back(kPrefix + joined);
        }
    }

    if (wantExt) {
        // 有扩展要加载时，去掉会一票否决的 --disable-extensions
        extraArgs.erase(
            std::remove(extraArgs.begin(), extraArgs.end(), std::string("--disable-extensions")),
            extraArgs.end());
        enableExt = true;
        // 未显式给 --load-extension 时由这里加上（用户自己给了就不重复）
        if (!userPassedLoadExt)
            extraArgs.push_back("--load-extension=" + extList);
        // 只允许这些扩展，防止 profile 里别的扩展干扰
        bool hasExcept = false;
        for (auto& a : extraArgs)
            if (a.rfind("--disable-extensions-except", 0) == 0) { hasExcept = true; break; }
        if (!hasExcept)
            extraArgs.push_back("--disable-extensions-except=" + extList);
    }

    // ------------------------------------------------------------------
    // 必备开关：这三个是启动稳定性的底线，缺了就补
    //   --disable-crash-reporter / --disable-breakpad
    //       Crashpad 起不来时 Chrome 会 ImmediateCrash 自杀（0x80000003 /
    //       0xFFFF7001），关掉崩溃上报可以绕开这一层。
    //   --no-sandbox
    //       Chrome 沙箱与部分安全软件 / 受限环境冲突，会直接起不来。
    // 检测的是真正要用的 extraArgs（= config 的 chromeArgs 加工后的结果），
    // 支持 "--flag" 和 "--flag=value" 两种写法；已有的话不重复加。
    // ------------------------------------------------------------------
    {
        static const char* kRequired[] = {
            "--disable-crash-reporter",
            "--disable-breakpad",
            "--no-sandbox",
            "--disable-features=LocalNetworkAccessChecks",
            "--start-maximized","--disable-desktop-notifications",
            "--disable-gpu-vsync",
            "--enable-print-preview"
        };
        std::string added;
        for (const char* sw : kRequired) {
            const std::string key(sw);
            bool present = false;
            for (const auto& a : extraArgs) {
                if (a.compare(0, key.size(), key) == 0 &&
                    (a.size() == key.size() || a[key.size()] == '=')) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                extraArgs.push_back(key);
                added += " ";
                added += key;
            }
        }
        if (!added.empty())
            spdlog::info("[Chrome] chromeArgs 缺少必备开关，自动补齐:{}", added);
    }

    // 组装命令行
    std::ostringstream cmd;
    cmd << "\"" << opts_.executablePath << "\"";
    cmd << " --remote-debugging-port=" << opts_.port;
    cmd << " --user-data-dir=\"" << opts_.userDataDir << "\"";
    if (opts_.headless) cmd << " --headless=new";
    if (opts_.disableGpu) cmd << " --disable-gpu";
    cmd << " --no-first-run";
    cmd << " --no-default-browser-check";
    if (!enableExt) cmd << " --disable-extensions";   // 配置了 --enable-extensions 则不禁用
    cmd << " --disable-background-networking";
    cmd << " --disable-default-apps";
    cmd << " --disable-sync";
    cmd << " --hide-scrollbars";
    cmd << " --metrics-recording-only";
    cmd << " --safebrowsing-disable-auto-update";
    cmd << " --password-store=basic";
    cmd << " --use-mock-keychain";
    // 追加配置里的 Chrome 启动参数（含空格的用引号包裹，如 user-agent）
    for (auto& a : extraArgs) {
        if (a.empty()) continue;
        if (a.find(' ') != std::string::npos && a.front() != '"')
            cmd << " \"" << a << "\"";
        else
            cmd << " " << a;
    }
    cmd << " about:blank";

    std::string cmdStr = cmd.str();
    if (wantExt)
        spdlog::info("[Chrome] 扩展加载: {} 个 -> {}", extDirs.size(), extList);
    spdlog::info("[Chrome] Launch: {}", cmdStr);

    // ------------------------------------------------------------------
    // 启动：两阶段尝试
    //   阶段 1「完整参数」 —— prefs + 扩展 + 配置里的 chromeArgs（+ 必备开关）
    //   阶段 2「最小参数」 —— 只保留让 CDP 起来的必需项（同样带必备开关）
    // 三个必备开关已经在上面统一补齐，所以这里只保留两个阶段：
    // 再单开一个「+关崩溃上报」的阶段会与阶段 1 完全重复。
    // ------------------------------------------------------------------
    std::ostringstream minimal;
    minimal << "\"" << opts_.executablePath << "\""
            << " --remote-debugging-port=" << opts_.port
            << " --user-data-dir=\"" << opts_.userDataDir << "\""
            << " --no-first-run --no-default-browser-check"
            << " --disable-crash-reporter --disable-breakpad --no-sandbox"
            << " about:blank";

#if defined(_WIN32)
    DWORD lastExitCode = 0;

    auto runAttempt = [&](const char* name, const std::string& cmd,
                          int timeoutSec) -> bool {
        spdlog::info("[Chrome] 启动({}): {}", name, cmd);

        // CreateProcessW 需要可写缓冲区（会就地改命令行）
        std::wstring wcmd = Utf8ToWide(cmd);
        std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        si.wShowWindow = SW_SHOWMAXIMIZED;
        if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &si, &pi)) {
            spdlog::error("[Chrome] CreateProcessW 失败 ({}), err={}", name,
                          GetLastError());
            return false;
        }
        CloseHandle(pi.hThread);

        // pi.hProcess 就是我们要的存活检测句柄，不必再 OpenProcess(pid)
        // —— 受限令牌下 OpenProcess 自己子进程都可能被拒，这正是旧代码误判的源头
        auto started = std::chrono::steady_clock::now();
        auto deadline = started + std::chrono::seconds(timeoutSec);
        bool exited = false;
        DWORD exitCode = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            if (CdpPortReady(opts_.port)) {
                long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
                spdlog::info("[Chrome] CDP ready (pid={}, {}ms, {})",
                             pi.dwProcessId, ms, name);
                pid_ = pi.dwProcessId;
                CloseHandle(pi.hProcess);
                launched_ = true;
                return true;
            }
            if (GetExitCodeProcess(pi.hProcess, &exitCode) &&
                exitCode != STILL_ACTIVE) {
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
        if (exited) {
            lastExitCode = exitCode;
            spdlog::error("[Chrome] Chrome 启动后 {}ms 即退出, exitCode=0x{:08X} ({})",
                          static_cast<long long>(elapsedMs),
                          static_cast<unsigned>(exitCode), name);
            if (elapsedMs < 3000) {
                spdlog::error(
                    "[Chrome] 「秒退」不是命令行拼错，而是 Chrome 在初始化 Crashpad "
                    "时失败后主动自杀（常见 0x80000003 / 0xFFFF7001，stderr 会看到 "
                    "crashpad_client_win.cc OpenProcess 拒绝访问）。");
            }
        } else {
            spdlog::warn("[Chrome] CDP 端口 {} 在 {}s 内未就绪 ({})", opts_.port,
                         timeoutSec, name);
        }

        // 清掉本次尝试的进程，别让它继续占着 profile / 端口，影响下一阶段
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pi.dwProcessId);
        if (h) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 3000);
            CloseHandle(h);
        }
        CloseHandle(pi.hProcess);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return false;
    };

    if (runAttempt("完整参数", cmdStr, 30)) return true;
    if (runAttempt("最小参数", minimal.str(), 15)) return true;

    spdlog::error(
        "[Chrome] 完整参数与最小参数都起不来 —— 不是 Launch() 的拼接问题。"
        " 请先手工验证这条命令能不能开 CDP: {}",
        cmdStr);
    (void)lastExitCode;
    return false;
#else
    pid_ = fork();
    if (pid_ == 0) {
        // child
        execl("/bin/sh", "sh", "-c", cmdStr.c_str(), nullptr);
        _exit(1);
    }
    if (pid_ < 0) {
        spdlog::error("[Chrome] fork failed");
        return false;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (CdpPortReady(opts_.port)) {
            launched_ = true;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    spdlog::error("[Chrome] CDP port not ready after 30s");
    Shutdown();
    return false;
#endif
}

void ChromeLauncher::Shutdown() {
    if (!launched_ && pid_ == 0) return;
#if defined(_WIN32)
    if (pid_) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid_);
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
            spdlog::info("[Chrome] Terminated pid {}", pid_);
        }
    }
#else
    if (pid_) { kill(pid_, SIGTERM); pid_ = 0; }
#endif
    launched_ = false;
    pid_ = 0;
}

// ----------------------------------------------------------------------------
// ChromeBrowser
// ----------------------------------------------------------------------------

ChromeBrowser::ChromeBrowser(int port) : port_(port) {}
ChromeBrowser::~ChromeBrowser() = default;

std::vector<ChromeTarget> ChromeBrowser::ListTargets() {
    httplib::Client cli("127.0.0.1", port_);
    cli.set_connection_timeout(2);
    auto resp = cli.Get("/json/list");
    std::vector<ChromeTarget> result;
    if (!resp || resp->status != 200) {
        spdlog::error("[Chrome] /json/list failed");
        return result;
    }

    auto arr = json::parse(resp->body);
    for (auto& t : arr) {
        ChromeTarget ct;
        ct.id    = t.value("id", "");
        ct.title = t.value("title", "");
        ct.url   = t.value("url", "");
        ct.type  = t.value("type", "");
        ct.wsUrl = t.value("webSocketDebuggerUrl", "");
        result.push_back(std::move(ct));
    }
    return result;
}

ChromeTarget ChromeBrowser::CreatePage(const std::string& url) {
    httplib::Client cli("127.0.0.1", port_);
    cli.set_connection_timeout(2);
    // Chrome 156+ 要求 PUT /json/new 才能创建新 page
    std::string path = "/json/new?";
    std::string encoded;
    for (char c : url) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ':' || c == '/' || c == '?')
            encoded += c;
        else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            encoded += buf;
        }
    }
    auto resp = cli.Put(path + encoded, "", "text/plain");
    if (!resp || resp->status != 200) {
        spdlog::error("[Chrome] /json/new failed, status={}",
                      resp ? resp->status : -1);
        throw std::runtime_error("Chrome /json/new failed");
    }

    auto t = json::parse(resp->body);
    ChromeTarget ct;
    ct.id    = t.value("id", "");
    ct.title = t.value("title", "");
    ct.url   = t.value("url", "");
    ct.type  = t.value("type", "page");
    ct.wsUrl = t.value("webSocketDebuggerUrl", "");
    return ct;
}

ChromeTarget ChromeBrowser::GetOrCreatePage(const std::string& url) {
    auto targets = ListTargets();
    for (auto& t : targets)
        if (t.type == "page" && t.title.find("New Rpa") == std::string::npos
            && !t.url.empty())  // 找一个非 about:blank 的
            return t;
    return CreatePage(url);
}

std::unique_ptr<CDPSession> ChromeBrowser::Connect(const ChromeTarget& target) {
    auto sess = std::make_unique<CDPSession>(target.wsUrl);
    if (!sess->Connect()) return nullptr;
    return sess;
}

std::unique_ptr<CDPSession> ChromeBrowser::Open(const std::string& url) {
    auto page = CreatePage(url);
    return Connect(page);
}

// ----------------------------------------------------------------------------
// 查找 Chrome 可执行文件
// ----------------------------------------------------------------------------

std::string FindChromeExecutable() {
#if defined(_WIN32)
    // 常见安装路径
    const char* candidates[] = {
        R"(C:\Program Files\Google\Chrome\Application\chrome.exe)",
        R"(C:\Program Files (x86)\Google\Chrome\Application\chrome.exe)",
        R"(C:\Program Files\Microsoft\Edge\Application\msedge.exe)",
        R"(C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe)",
        nullptr
    };
    for (int i = 0; candidates[i]; ++i)
        if (fs::exists(candidates[i])) return candidates[i];

    // 注册表查找
    HKEY hk = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      R"(SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\chrome.exe)",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        char buf[MAX_PATH]{};
        DWORD n = sizeof(buf);
        if (RegQueryValueExA(hk, nullptr, nullptr, nullptr,
                            (LPBYTE)buf, &n) == ERROR_SUCCESS) {
            RegCloseKey(hk);
            if (fs::exists(buf)) return buf;
        }
        RegCloseKey(hk);
    }
    return "";
#else
    const char* candidates[] = {
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/snap/bin/chromium",
        "/usr/bin/chromium-browser",
        nullptr
    };
    for (int i = 0; candidates[i]; ++i)
        if (access(candidates[i], X_OK) == 0) return candidates[i];
    return "";
#endif
}

} // namespace chrome
} // namespace newrpa
