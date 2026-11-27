# bcb plugin — 北京银行企业网银

add_library(bcb_plugin SHARED
    BCBPlugin.cpp
    strategy/SliderVision.cpp
    strategy/DingXiangSlider.cpp
    strategy/ClickWordCaptcha.cpp
    variant/BcbVariant.cpp
    ../../core/src/singleUsbbox/HidKeyBoard.cpp
    ../../core/src/singleUsbbox/hid_box.cpp
    ../../core/src/singleUsbbox/HidMouse.cpp
)

target_include_directories(bcb_plugin PRIVATE
    ../../core/src/singleUsbbox
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
