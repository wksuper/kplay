
add_executable(kplay
    kplay.cpp
)
target_link_libraries(kplay
    lark
    klogging
)

install(
    TARGETS
        kplay
)
