# bcb plugin — 北京银行企业网银

add_library(bcb_plugin SHARED
    BCBPlugin.cpp
)

set_target_properties(bcb_plugin PROPERTIES
    OUTPUT_NAME "bcb"
    SUFFIX ".dll"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)

target_link_libraries(bcb_plugin
    PRIVATE NewRpaCore NewRpaPluginApi winhttp
)

install(TARGETS bcb_plugin
        RUNTIME DESTINATION bin/plugins
        LIBRARY DESTINATION bin/plugins
)
