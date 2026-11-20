#pragma once
// ============================================================================
// Config — 配置加载（JSON / INI）
// ============================================================================
#include "newrpa/plugin/IBankPlugin.h"
#include <string>
#include <vector>

namespace newrpa {
namespace util {

struct GlobalConfig {
    // Chrome 启动
    std::string chromePath;     // 空 = 自动查找
    int         chromePort = 9222;
    std::string chromeProfileDir; // 空 = 临时目录
    bool        headless = false;

    // Chrome 启动参数（对应 Selenium add_argument）
    std::vector<std::string> chromeArgs;
    // Chrome 实验选项 prefs（对应 add_experimental_option("prefs", {...})）
    // 用 json 对象保存，键为点号路径如 "download.prompt_for_download"
    std::string chromePrefsJson;   // 原始 JSON 字符串，启动时解析写入 Preferences 文件
    // Selenium excludeSwitches（记录用途，CDP 直启无 ChromeDriver 等价物）
    std::vector<std::string> chromeExcludeSwitches;

    // 插件目录
    std::string pluginDir = "./plugins";

    // HTTP 心跳（保留旧项目 qihangjinfu 风格）
    std::string heartbeatUrl;
    int         heartbeatIntervalSec = 3600;

    // 日志
    std::string logLevel = "info";
    std::string logFile;          // 空 = 控制台

    // 银行列表
    std::vector<plugin::BankConfig> banks;
};

// 从 config.json 加载
GlobalConfig LoadConfig(const std::string& path);

// 写一份默认 config.json 出来
void SaveDefaultConfig(const std::string& path);

} // namespace util
} // namespace newrpa
