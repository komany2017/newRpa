#pragma once

#include "newrpa/NewRpaExport.h"
// ============================================================================
// ChromeBrowser — 管理一个运行中的 Chrome 实例 + 标签页
// ChromeLauncher — 独立启动 Chrome 进程
// ============================================================================

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace newrpa {
namespace cdp { class CDPSession; } // 前向声明
namespace chrome {

using newrpa::cdp::CDPSession;

struct ChromeTarget {
    std::string id;         // Target id，拼在 ws URL 里
    std::string title;
    std::string url;
    std::string type;       // "page" / "iframe" / "service_worker" ...
    std::string wsUrl;      // 完整 WebSocket URL
};

// 启动一个 Chrome 进程，返回 ChromeBrowser 管理对象
// executablePath: 可为空，默认自动查找 Chrome/Edge
// port: --remote-debugging-port，默认 9222
// userDataDir: 独立 profile，避免和用户日常 Chrome 冲突
class NEWRPA_API ChromeLauncher {
public:
    struct Options {
        std::string executablePath;
        int         port         = 9222;
        std::string userDataDir;       // 空 = 临时目录，关闭时删除
        bool        headless     = false;
        bool        disableGpu   = true;
        std::string initialUrl   = "about:blank";
        bool        keepRunning  = false;  // 析构时是否杀进程

        // 追加的 Chrome 命令行参数（来自配置 chromeArgs）
        std::vector<std::string> extraArgs;

        // 要加载的解压版扩展目录（绝对路径，来自配置 extensionPaths）
        // 启动器会拼 --load-extension / --disable-extensions-except，
        // 并在 Chrome ≥137 时自动补 DisableLoadExtensionCommandLineSwitch 开关
        std::vector<std::string> extensions;
        // Chrome Preferences 覆盖（点号路径 JSON，来自配置 chromePrefs）
        // 启动时合并写入 <userDataDir>/Default/Preferences
        std::string prefsJson;
    };

    explicit ChromeLauncher(const Options& opts);
    ~ChromeLauncher();

    ChromeLauncher(const ChromeLauncher&) = delete;
    ChromeLauncher& operator=(const ChromeLauncher&) = delete;

    bool Launch();        // 阻塞等 --remote-debugging-port 就绪
    void Shutdown();      // 杀进程

    uint32_t GetPid()      const { return pid_; }
    int      GetPort()     const { return opts_.port; }
    const std::string& GetWsPrefix() const { return wsPrefix_; }

private:
    Options   opts_;
    uint32_t  pid_         = 0;
    bool      launched_    = false;
    std::string wsPrefix_;            // "ws://127.0.0.1:<port>/devtools"
};

// 连接到一个已运行的 Chrome（或由 ChromeLauncher 启动的）
class NEWRPA_API ChromeBrowser {
public:
    explicit ChromeBrowser(int port = 9222);
    ~ChromeBrowser();

    // 通过 HTTP GET http://127.0.0.1:<port>/json/list 获取所有 targets
    std::vector<ChromeTarget> ListTargets();

    // 创建新页面（通过 /json/new?url= 或直接 CDP Target.createTarget）
    ChromeTarget CreatePage(const std::string& url = "about:blank");

    // 找到或创建一个 type=="page" 的 target
    ChromeTarget GetOrCreatePage(const std::string& url = "about:blank");

    // 连接到指定 target，返回 CDPSession
    std::unique_ptr<CDPSession> Connect(const ChromeTarget& target);

    // 便捷版：直接打开 URL，返回 CDPSession
    std::unique_ptr<CDPSession> Open(const std::string& url);

    int GetPort() const { return port_; }

private:
    int port_;
};

// 在系统里搜索 Chrome / Edge 可执行文件路径
NEWRPA_API std::string FindChromeExecutable();

} // namespace chrome
} // namespace newrpa
