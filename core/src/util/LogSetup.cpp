// ============================================================================
// LogSetup — 同时输出到控制台和按天分割的日志文件
// 插件与核心库共用同一个 spdlog 默认 logger，因此 [BCB]/[CDP] 等日志也会落盘。
// ============================================================================
#include "newrpa/util/LogSetup.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace newrpa {
namespace util {

// spdlog daily sink 会在 basename 与扩展名之间插入 _YYYY-MM-DD
static fs::path DatedLogPath(const fs::path& base) {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char date[16] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d", &tm);
    return base.parent_path() / (base.stem().string() + "_" + date + base.extension().string());
}

void SetupLogging(const std::string& logFile, const char* argv0) {
    const char* pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v";

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    fs::path path = logFile.empty()
        ? (fs::absolute(argv0).parent_path() / "logs" / "newrpa.log")
        : fs::path(logFile);
    if (!path.is_absolute()) path = fs::absolute(path);

    bool fileOk = false;
    try {
        fs::create_directories(path.parent_path());
        // 每天 00:00 切换一个日志文件，文件名形如 newrpa_2026-11-20.log
        // truncate=false：同一天多次运行追加到同一文件；最多保留 30 天
        sinks.push_back(std::make_shared<spdlog::sinks::daily_file_sink_mt>(
            path.string(), 0, 0, false, 30));
        fileOk = true;
    } catch (const std::exception& e) {
        std::cerr << "Log file unavailable (" << path.string()
                  << "): " << e.what() << "; console only\n";
    }

    auto logger = std::make_shared<spdlog::logger>("newrpa", sinks.begin(), sinks.end());
    logger->set_pattern(pattern);
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);

    if (fileOk) spdlog::info("Log file: {}", DatedLogPath(path).string());
}

} // namespace util
} // namespace newrpa
