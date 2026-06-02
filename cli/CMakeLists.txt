cmake_minimum_required(VERSION 3.20)
project(drape_cli CXX)

# ── Shared NIF + OSD libraries (from BodySlide build) ────────────────
set(BS_SRC  "E:/SkyrimDev/BodySlide-and-Outfit-Studio/src")
set(BS_DEPS "E:/SkyrimDev/BodySlide-and-Outfit-Studio/lib")

add_executable(drape_cli
    main.cpp
    DrapeConformer.cpp
)

set_target_properties(drape_cli PROPERTIES OUTPUT_NAME "drape-cli")
target_compile_features(drape_cli PRIVATE cxx_std_17)

target_include_directories(drape_cli PRIVATE
    ${BS_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link against the same libs as mppatcher (nifly, zlib, etc.)
# Adjust paths if BodySlide build dir differs
target_link_directories(drape_cli PRIVATE
    "E:/SkyrimDev/BodySlide-and-Outfit-Studio/mppatcher/build/Release"
)
