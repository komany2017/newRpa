#pragma once
// ============================================================================
// LogSetup — 控制台 + 按天分割的日志文件初始化
// 实现放在 NewRpaCore.dll 内，避免 exe 再链一份 spdlog 静态库导致符号重复。
// ============================================================================
#include <string>

namespace newrpa {
namespace util {

// logFile 为空时使用 <exe目录>/logs/newrpa.log（实际文件自动加日期后缀）
// argv0 用于定位 exe 所在目录
void SetupLogging(const std::string& logFile, const char* argv0);

} // namespace util
} // namespace newrpa
