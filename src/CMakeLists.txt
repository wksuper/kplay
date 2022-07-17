
add_executable(kplay
    kplay.cpp
)
target_link_libraries(kplay
    lark
    klogging
    pthread
)

install(
    TARGETS
        kplay
)
