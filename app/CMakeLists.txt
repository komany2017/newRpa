# NewRpa CLI — 主控程序

add_executable(newrpa
    main.cpp
)

target_link_libraries(newrpa PRIVATE NewRpaCore)

install(TARGETS newrpa RUNTIME DESTINATION bin)
