# NewRpa CLI — 主控程序

add_executable(newrpa
    main.cpp
)

target_link_libraries(newrpa PRIVATE NewRpaCore winhttp)

install(TARGETS newrpa RUNTIME DESTINATION bin)

# ----------------------------------------------------------------------------
# desbase64 — DesBase64 编解码小工具（独立 exe，不依赖 NewRpaCore）
# ----------------------------------------------------------------------------
add_executable(desbase64
    tools/desbase64_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/src/des/DesBase64.cpp
)

# 头文件在 core/src/des/ 下，源码在 app/tools/ 下
target_include_directories(desbase64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/src/des
)

# 静态链 libgcc/libstdc++，exe 拷到任何机器都能直接跑
if(MINGW)
    target_link_options(desbase64 PRIVATE
        -static-libgcc -static-libstdc++ -static)
endif()

install(TARGETS desbase64 RUNTIME DESTINATION bin)
