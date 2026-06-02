cmake_minimum_required(VERSION 3.20)
project(drape_proxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BS_ROOT "E:/SkyrimDev/BodySlide-and-Outfit-Studio")
set(MPP_DIR "${BS_ROOT}/mppatcher")

add_library(drape_proxy SHARED
    dllmain.cpp
    version_proxy.cpp
    dialog.cpp
    version.def
)

set_target_properties(drape_proxy PROPERTIES
    OUTPUT_NAME "version"
    PREFIX      ""
    SUFFIX      ".dll"
)

target_include_directories(drape_proxy PRIVATE
    "${BS_ROOT}/src/utils"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(drape_proxy PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN64
)

target_link_libraries(drape_proxy PRIVATE
    kernel32 user32 shell32 comctl32 comdlg32
)
