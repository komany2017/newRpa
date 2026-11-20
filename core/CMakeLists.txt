# ============================================================
# NewRpa Core Library — 编译成 NewRpaCore.dll / .lib
#
# 纯 C++17 + Win32 API，零外部网络库依赖（MinWS 自实现）
# ============================================================
add_library(NewRpaCore SHARED
    src/cdp/CDPSession.cpp
    src/cdp/CDPDomains.cpp
    src/ws/MinWS.cpp
    src/chrome/ChromeBrowser.cpp    # 含 ChromeLauncher 实现
    src/plugin/PluginLoader.cpp
    src/util/Config.cpp              # 含 HttpClient/ExcelWriter/Crypto 桩
    src/util/StringUtils.cpp
    src/util/LogSetup.cpp            # 控制台 + 滚动日志文件
)

target_include_directories(NewRpaCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(NewRpaCore
    PRIVATE NEWRPA_BUILD
    PUBLIC  NEWRPA_USE_DLL
)

target_link_libraries(NewRpaCore
    PUBLIC
        nlohmann_json::nlohmann_json
        httplib::httplib
        spdlog::spdlog
)

if(WIN32)
    target_link_libraries(NewRpaCore PRIVATE ws2_32 mswsock)
    if(MINGW)
        # MinGW 下 CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 不生效，必须显式导出
        target_link_options(NewRpaCore PRIVATE
            -static-libgcc -static-libstdc++
            -Wl,--export-all-symbols
        )
    endif()
endif()

# 插件接口头文件（单独的 INTERFACE 目标，插件只要链接这个）
add_library(NewRpaPluginApi INTERFACE)
target_include_directories(NewRpaPluginApi INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(NewRpaPluginApi INTERFACE nlohmann_json::nlohmann_json)

install(TARGETS NewRpaCore NewRpaPluginApi EXPORT NewRpaTargets
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
        ARCHIVE DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)
