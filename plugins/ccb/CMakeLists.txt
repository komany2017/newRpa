# ccb plugin — 中国建设银行企业网银（DemoV3 演示版）

add_library(ccb_plugin SHARED
    CCBPlugin.cpp
)

target_include_directories(ccb_plugin PRIVATE
    ../../core/include
    ../../third_party/nlohmann_json/include
    ../../third_party/spdlog/include
)

set_target_properties(ccb_plugin PROPERTIES
    OUTPUT_NAME "ccb"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)

target_link_libraries(ccb_plugin
    PRIVATE NewRpaCore
)

install(TARGETS ccb_plugin
        RUNTIME DESTINATION bin/plugins
        LIBRARY DESTINATION bin/plugins)
