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
    ../tools/desbase64_main.cpp
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

# ----------------------------------------------------------------------------
# mouse_move — 单头盒子硬件鼠标 CLI（HidMouse 自测 / 调试工具）
# ----------------------------------------------------------------------------
add_executable(mouse_move
    ../tools/mouse_move_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/src/singleUsbbox/HidMouse.cpp
)

target_include_directories(mouse_move PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/src/singleUsbbox            # HidMouse.cpp: #include "HidMouse.hpp"
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include                     # mouse_move_main.cpp: "newrpa/singleUsbbox/HidMouse.hpp"
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include/newrpa/singleUsbbox # 兜底：裸名包含
)

# 静态链，exe 拷走即用
if(MINGW)
    target_link_options(mouse_move PRIVATE
        -static-libgcc -static-libstdc++ -static)
endif()

# 纯 HID 实现，不再依赖 msdk.dll（无 POST_BUILD 复制）

install(TARGETS mouse_move RUNTIME DESTINATION bin)
