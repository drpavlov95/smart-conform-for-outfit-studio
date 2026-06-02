cmake_minimum_required(VERSION 3.20)
project(drape_proxy CXX)

add_library(drape_proxy SHARED
    dllmain.cpp
    version_proxy.cpp
)

# Output as "version.dll" (no lib prefix, no extra suffix)
set_target_properties(drape_proxy PROPERTIES
    OUTPUT_NAME "version"
    PREFIX      ""
    SUFFIX      ".dll"
)

target_compile_features(drape_proxy PRIVATE cxx_std_17)
target_link_libraries(drape_proxy PRIVATE kernel32 user32 shell32)
