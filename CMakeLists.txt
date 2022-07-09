project(kplay)
cmake_minimum_required(VERSION 3.1)

set(CMAKE_CXX_STANDARD 11)
include_directories(/usr/local/include)
link_directories(/usr/local/lib)
add_subdirectory(src)
