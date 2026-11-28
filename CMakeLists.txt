# ============================================================
# NewRpa-Next — C++20 + CDP + Chrome 网银 RPA 框架
#
# MinGW-w64 目标 (MSYS2 / WinLibs / MinGW-Builds 发行包都行)
# ============================================================
cmake_minimum_required(VERSION 3.24)
project(NewRpaNext LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出编译数据库（compile_commands.json）—— VSCode 跳转/补全靠它精确定位
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows DLL 导出所有符号（MinGW/MSVC 通用）
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(NEWRPA_BUILD_PLUGINS "Build bank plugins" ON)
option(NEWRPA_BUILD_APP     "Build host application" ON)

# ------------------------------------------------------------
# 第三方库（本地 third_party/，避免 GitHub 拉取）
# ------------------------------------------------------------
set(TP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# nlohmann/json —— header-only，带 CMakeLists.txt
add_subdirectory("${TP_DIR}/nlohmann_json"
                 "${CMAKE_BINARY_DIR}/_third_party/nlohmann_json")

# cpp-httplib —— header-only，带 CMakeLists.txt
add_subdirectory("${TP_DIR}/cpp_httplib"
                 "${CMAKE_BINARY_DIR}/_third_party/cpp_httplib")

# spdlog —— 带 CMakeLists.txt，关 SHARED
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP_DIR}/spdlog"
                 "${CMAKE_BINARY_DIR}/_third_party/spdlog")

# OpenSSL —— 可选（crypto 桩暂时不用）
find_package(OpenSSL 3.0 QUIET)
if(NOT OpenSSL_FOUND)
    message(STATUS "OpenSSL not found; crypto utilities will be stubbed.")
endif()

# ------------------------------------------------------------
# 子模块
# ------------------------------------------------------------
add_subdirectory(core)

if(NEWRPA_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

if(NEWRPA_BUILD_APP)
    add_subdirectory(app)
endif()
