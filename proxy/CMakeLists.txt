cmake_minimum_required(VERSION 3.20)
project(drape_proxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BS_ROOT "E:/SkyrimDev/BodySlide-and-Outfit-Studio")
set(MPP_DIR "${BS_ROOT}/mppatcher")

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
    "${BS_ROOT}/src/components/DiffData.cpp"
    "${BS_ROOT}/src/components/SliderData.cpp"
    "${BS_ROOT}/src/utils/PlatformUtil.cpp"
    "${BS_ROOT}/src/utils/StringStuff.cpp"
    "${BS_ROOT}/src/utils/AABBTree.cpp"
    "${BS_ROOT}/src/utils/ConfigurationManager.cpp"
    "${MPP_DIR}/stubs/Log_stub.cpp"
)

add_library(drape_proxy SHARED
    dllmain.cpp
    version_proxy.cpp
    dialog.cpp
    ../cli/DrapeConformer.cpp
    ${NIFLY_SRCS}
    ${BS_SRCS}
)

set_target_properties(drape_proxy PROPERTIES
    OUTPUT_NAME "version"
    PREFIX      ""
    SUFFIX      ".dll"
)

target_include_directories(drape_proxy PRIVATE
    "${MPP_DIR}/wx_stubs"
    "${BS_ROOT}/lib/nifly/include"
    "${BS_ROOT}/lib/nifly/external"
    "${BS_ROOT}/lib/nifly/src"
    "${BS_ROOT}/src"
    "${BS_ROOT}/src/components"
    "${BS_ROOT}/src/utils"
    "${BS_ROOT}/lib"
    "${BS_ROOT}/lib/gli"
    "${BS_ROOT}/lib/gli/glm"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cli"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(drape_proxy PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN64
)

target_link_libraries(drape_proxy PRIVATE
    kernel32 user32 shell32 comctl32 comdlg32 opengl32
)
