# cmb plugin — 招商银行非证书用户（U-Bank）

add_library(cmb_plugin SHARED
    CMBPlugin.cpp
)

target_include_directories(cmb_plugin PRIVATE
    ../../core/include
    ../../third_party/nlohmann_json/include
    ../../third_party/spdlog/include
)

set_target_properties(cmb_plugin PROPERTIES
    OUTPUT_NAME "cmb"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)

target_link_libraries(cmb_plugin
    PRIVATE NewRpaCore
)

install(TARGETS cmb_plugin
        RUNTIME DESTINATION bin/plugins
        LIBRARY DESTINATION bin/plugins)
