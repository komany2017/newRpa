# icbc plugin — 中国工商银行

add_library(icbc_plugin SHARED
    ICBCPlugin.cpp
)

set_target_properties(icbc_plugin PROPERTIES
    OUTPUT_NAME "icbc"
    SUFFIX ".dll"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)

target_link_libraries(icbc_plugin
    PRIVATE NewRpaCore NewRpaPluginApi
)

install(TARGETS icbc_plugin
        RUNTIME DESTINATION bin/plugins
        LIBRARY DESTINATION bin/plugins
)
