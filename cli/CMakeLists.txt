cmake_minimum_required(VERSION 3.20)
project(drape_cli CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BS_ROOT "E:/SkyrimDev/BodySlide-and-Outfit-Studio")
set(MPP_DIR "${BS_ROOT}/mppatcher")

# ── nifly sources (NIF I/O) ───────────────────────────────────────────
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

# ── BodySlide components needed for OSD I/O ───────────────────────────
set(BS_SRCS
    "${BS_ROOT}/lib/TinyXML-2/tinyxml2.cpp"
    "${BS_ROOT}/src/components/DiffData.cpp"
    "${BS_ROOT}/src/components/SliderData.cpp"
    "${BS_ROOT}/src/utils/PlatformUtil.cpp"
    "${BS_ROOT}/src/utils/StringStuff.cpp"
    "${BS_ROOT}/src/utils/AABBTree.cpp"
    "${BS_ROOT}/src/utils/ConfigurationManager.cpp"
    # Stubs: replace wx-coupled Log + Config with no-ops
    "${MPP_DIR}/stubs/Log_stub.cpp"
    "${MPP_DIR}/stubs/Config_stub.cpp"
)

add_executable(drape_cli
    main.cpp
    DrapeConformer.cpp
    ${NIFLY_SRCS}
    ${BS_SRCS}
)

set_target_properties(drape_cli PROPERTIES OUTPUT_NAME "drape-cli")

target_include_directories(drape_cli PRIVATE
    # wx stubs FIRST — must override real wx headers
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
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(drape_cli PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN64
)

target_link_libraries(drape_cli PRIVATE opengl32)
