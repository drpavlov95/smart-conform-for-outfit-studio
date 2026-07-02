cmake_minimum_required(VERSION 3.20)
project(drape_algo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libigl (header-only, MPL-2.0) + Eigen (MPL-2.0) for the ARAP deformation core
# (plan Phase 1b). Both are header-only; no separate build.
set(LIBIGL_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../reference/libigl/include")
set(EIGEN_INCLUDE   "${CMAKE_CURRENT_SOURCE_DIR}/../reference/eigen")
if (NOT EXISTS "${LIBIGL_INCLUDE}/igl/arap.h")
    message(FATAL_ERROR "libigl not found at ${LIBIGL_INCLUDE}")
endif()
if (NOT EXISTS "${EIGEN_INCLUDE}/Eigen/Core")
    message(FATAL_ERROR "Eigen not found at ${EIGEN_INCLUDE}")
endif()

set(BS_ROOT "E:/SkyrimDev/BodySlide-and-Outfit-Studio" CACHE PATH
    "Path to a BodySlide and Outfit Studio source tree used for nifly/DiffData sources")
set(MPP_DIR "${BS_ROOT}/mppatcher")

if (NOT EXISTS "${BS_ROOT}/src/components/DiffData.cpp")
    message(FATAL_ERROR "BS_ROOT does not look like a BodySlide source tree: ${BS_ROOT}")
endif()

if (NOT EXISTS "${MPP_DIR}/stubs/Config_stub.cpp")
    message(FATAL_ERROR "mppatcher stubs not found under BS_ROOT: ${MPP_DIR}")
endif()

set(NIFLY_SRCS
    "${BS_ROOT}/lib/nifly/src/Animation.cpp"
    "${BS_ROOT}/lib/nifly/src/BasicTypes.cpp"
    "${BS_ROOT}/lib/nifly/src/bhk.cpp"
    "${BS_ROOT}/lib/nifly/src/ExtraData.cpp"
    "${BS_ROOT}/lib/nifly/src/Factory.cpp"
    "${BS_ROOT}/lib/nifly/src/Geometry.cpp"
    "${BS_ROOT}/lib/nifly/src/NifFile.cpp"
    "${BS_ROOT}/lib/nifly/src/NifUtil.cpp"
    "${BS_ROOT}/lib/nifly/src/Nodes.cpp"
    "${BS_ROOT}/lib/nifly/src/Object3d.cpp"
    "${BS_ROOT}/lib/nifly/src/Objects.cpp"
    "${BS_ROOT}/lib/nifly/src/Particles.cpp"
    "${BS_ROOT}/lib/nifly/src/Shaders.cpp"
    "${BS_ROOT}/lib/nifly/src/Skin.cpp"
)

set(BS_SRCS
    "${BS_ROOT}/lib/TinyXML-2/tinyxml2.cpp"
    "${BS_ROOT}/src/components/DiffData.cpp"
    "${BS_ROOT}/src/components/SliderData.cpp"
    "${BS_ROOT}/src/utils/PlatformUtil.cpp"
    "${BS_ROOT}/src/utils/StringStuff.cpp"
    "${BS_ROOT}/src/utils/AABBTree.cpp"
    "${BS_ROOT}/src/utils/ConfigurationManager.cpp"
    "${MPP_DIR}/stubs/Config_stub.cpp"
    "${MPP_DIR}/stubs/Log_stub.cpp"
)

set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/minhook")
set(MINHOOK_SRCS
    "${MINHOOK_DIR}/src/buffer.c"
    "${MINHOOK_DIR}/src/hook.c"
    "${MINHOOK_DIR}/src/trampoline.c"
    "${MINHOOK_DIR}/src/hde/hde32.c"
    "${MINHOOK_DIR}/src/hde/hde64.c"
)

add_library(drape_algo SHARED
    algo.cpp
    conform_hook.cpp
    skin_arap.cpp
    ../cli/DrapeConformer.cpp
    ${MINHOOK_SRCS}
    ${NIFLY_SRCS}
    ${BS_SRCS}
)

set_target_properties(drape_algo PROPERTIES
    OUTPUT_NAME "drape-algo"
    PREFIX      ""
    SUFFIX      ".dll"
)

target_include_directories(drape_algo PRIVATE
    "${LIBIGL_INCLUDE}"
    "${EIGEN_INCLUDE}"
    "${MPP_DIR}/wx_stubs"
    "${BS_ROOT}/lib/nifly/include"
    "${BS_ROOT}/lib/nifly/external"
    "${BS_ROOT}/lib/nifly/src"
    "${BS_ROOT}/src"
    "${BS_ROOT}/src/components"
    "${BS_ROOT}/src/utils"
    "${BS_ROOT}/lib"
    "${BS_ROOT}/lib/TinyXML-2"
    "${BS_ROOT}/lib/gli"
    "${BS_ROOT}/lib/gli/glm"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cli"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proxy"
    "${MINHOOK_DIR}/include"
)

target_compile_definitions(drape_algo PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN64
)

target_link_libraries(drape_algo PRIVATE
    kernel32 user32 shell32 comctl32 opengl32
)

# Copy drape-algo.dll to BOTH mod folders that may shadow each other in the MO2
# VFS. `|| (exit 0)` so a locked file (OutfitStudio open) doesn't fail the build.
add_custom_command(TARGET drape_algo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:drape_algo>"
        "E:/Wabbajack/InstallPacks/mods/BodySlide and Outfit Studio/CalienteTools/BodySlide/drape-algo.dll"
        || ${CMAKE_COMMAND} -E true
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:drape_algo>"
        "E:/Wabbajack/InstallPacks/mods/Drape - Cloth Aware Conform/CalienteTools/BodySlide/drape-algo.dll"
        || ${CMAKE_COMMAND} -E true
    COMMENT "Deploying drape-algo.dll to both OutfitStudio mod folders..."
)


add_executable(drape_harness
    conform_hook.cpp
    skin_arap.cpp
    ../cli/DrapeConformer.cpp
    ${NIFLY_SRCS}
    ${BS_SRCS}
)

set_target_properties(drape_harness PROPERTIES OUTPUT_NAME "drape-harness")

target_include_directories(drape_harness PRIVATE
    "${LIBIGL_INCLUDE}"
    "${EIGEN_INCLUDE}"
    "${MPP_DIR}/wx_stubs"
    "${BS_ROOT}/lib/nifly/include"
    "${BS_ROOT}/lib/nifly/external"
    "${BS_ROOT}/lib/nifly/src"
    "${BS_ROOT}/src"
    "${BS_ROOT}/src/components"
    "${BS_ROOT}/src/utils"
    "${BS_ROOT}/lib"
    "${BS_ROOT}/lib/TinyXML-2"
    "${BS_ROOT}/lib/gli"
    "${BS_ROOT}/lib/gli/glm"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cli"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proxy"
    "${MINHOOK_DIR}/include"
)

target_compile_definitions(drape_harness PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN64
    DRAPE_HARNESS_BUILD
)

target_link_libraries(drape_harness PRIVATE
    kernel32 user32 shell32 comctl32 opengl32
)

if (MSVC)
    target_compile_options(drape_algo PRIVATE /bigobj)
    target_compile_options(drape_harness PRIVATE /bigobj)
endif()
