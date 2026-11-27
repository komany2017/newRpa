# klb plugin — 昆仑银行企业网银（查询版）

add_library(klb_plugin SHARED
    KLBPlugin.cpp
)

# HID 键盘/鼠标已在 NewRpaCore 内编译（core/src/singleUsbbox + core/src/hid），
# 插件只链接 NewRpaCore，不再各自复制源码编译，也不需要额外 include 目录。
# （若某插件要直接用 HidKeyBoard/HidMouse 类，再自行加
#   ../../core/include/newrpa/singleUsbbox 即可。）

set_target_properties(klb_plugin PROPERTIES
    OUTPUT_NAME "klb"
    SUFFIX ".dll"
    # MinGW 下 DLL 算 RUNTIME 产物，只设 LIBRARY_OUTPUT_DIRECTORY 会落到 bin/ 而不是 bin/plugins/
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)

target_link_libraries(klb_plugin
    PRIVATE NewRpaCore NewRpaPluginApi
)

install(TARGETS klb_plugin
        RUNTIME DESTINATION bin/plugins
        LIBRARY DESTINATION bin/plugins
)
