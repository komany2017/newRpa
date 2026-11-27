# klb plugin — 昆仑银行企业网银（查询版）

add_library(klb_plugin SHARED
    KLBPlugin.cpp
    ../../core/src/singleUsbbox/HidKeyBoard.cpp
    ../../core/src/singleUsbbox/hid_box.cpp
)

target_include_directories(klb_plugin PRIVATE
    ../../core/src/singleUsbbox
    ../../core/include/newrpa/singleUsbbox   # HidKeyBoard.h / hid_box.hpp / ime_guard.hpp 已迁到此处
)

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
