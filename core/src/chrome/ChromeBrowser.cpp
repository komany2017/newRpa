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

ChromeLauncher::ChromeLauncher(const Options& opts) : opts_(opts) {
    if (opts_.port <= 0) opts_.port = 9222;
    if (opts_.executablePath.empty())
        opts_.executablePath = FindChromeExecutable();
    if (opts_.userDataDir.empty()) {
        // 建临时 profile 目录
        opts_.userDataDir = (fs::temp_directory_path() / "newrpa-chrome-profile").string();
        fs::create_directories(opts_.userDataDir);
        ownUserData_ = true;
    }
    wsPrefix_ = "ws://127.0.0.1:" + std::to_string(opts_.port) + "/devtools";
}

ChromeLauncher::~ChromeLauncher() {
    if (!opts_.keepRunning) Shutdown();
    if (ownUserData_ && !opts_.keepRunning) {
        try { fs::remove_all(opts_.userDataDir); } catch (...) {}
    }
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

bool ChromeLauncher::Launch() {
    if (launched_) return true;

    if (opts_.executablePath.empty()) {
        spdlog::error("[Chrome] No Chrome/Edge executable found");
        return false;
    }

    // 启动前写入 Chrome Preferences（必须在 Chrome 进程启动前落盘）
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

    // 处理 --disable-features 合并（DisableLoadExtensionCommandLineSwitch）
    std::vector<std::string> extraArgs = opts_.extraArgs;
    if (wantExt) {
        const char* kSwitch = "DisableLoadExtensionCommandLineSwitch";
        bool patched = false;
        for (auto& a : extraArgs) {
            if (a.rfind("--disable-features=", 0) == 0) {
                if (a.find(kSwitch) == std::string::npos) {
                    a += ","; a += kSwitch;      // 合并进用户已有的列表
                }
                patched = true;
                break;
            }
        }
        if (!patched)
            extraArgs.push_back(std::string("--disable-features=") + kSwitch);
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

#if defined(_WIN32)
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmdStr.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &si, &pi)) {
        spdlog::error("[Chrome] CreateProcess failed, err={}", GetLastError());
        return false;
    }
    pid_ = pi.dwProcessId;
    CloseHandle(pi.hThread);
    // 不 Close hProcess，Shutdown 时要用它
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
#endif

    // 等 /json/version 能响应
    spdlog::info("[Chrome] Waiting for CDP port {} ...", opts_.port);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    httplib::Client cli("127.0.0.1", opts_.port);
    cli.set_connection_timeout(1);
    while (std::chrono::steady_clock::now() < deadline) {
        auto resp = cli.Get("/json/version");
        if (resp && resp->status == 200) {
            spdlog::info("[Chrome] CDP ready (pid={})", pid_);
            launched_ = true;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    spdlog::error("[Chrome] CDP port not ready");
    Shutdown();
    return false;
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
        nullptrcs
    };
    for (int i = 0; candidates[i]; ++i)
        if (access(candidates[i], X_OK) == 0) return candidates[i];
    return "";
#endif
}

} // namespace chrome
} // namespace newrpa
