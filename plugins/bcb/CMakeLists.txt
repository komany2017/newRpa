# bcb plugin — 北京银行企业网银

add_library(bcb_plugin SHARED
    BCBPlugin.cpp
    ../../core/src/keyBoard/SkKeyBoard.cpp
    ../../core/src/keyBoard/hid_box.cpp
    ../../core/src/keyBoard/HidMouse.cpp
)

target_include_directories(bcb_plugin PRIVATE
    ../../core/src/keyBoard
    ../../core/include/newrpa/singleUsbbox
)

set_target_properties(bcb_plugin PROPERTIES
    OUTPUT_NAME "bcb"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)

target_link_libraries(bcb_plugin
    PRIVATE NewRpaCore NewRpaPluginApi winhttp gdiplus ole32
)

install(TARGETS bcb_plugin
        RUNTIME DESTINATION bin/plugins
        LIBRARY DESTINATION bin/plugins
)
