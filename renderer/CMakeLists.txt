# Hajinmon native renderer (experimental): renders the game with Plume
# (https://github.com/renderbag/plume) instead of the SDK's Xenos emulation.
# Built as a static library linked into the game; selected at run time with
# --renderer=plume.

set(HAJINMON_PLUME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/plume"
    CACHE PATH "Plume source tree (tools/setup_plume.cmd)")
set(HAJINMON_REXSDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/rexglue-sdk"
    CACHE PATH "ReXGlue SDK source tree")

if(NOT EXISTS "${HAJINMON_PLUME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Plume not found in ${HAJINMON_PLUME_DIR}: run tools\\setup_plume.cmd")
endif()
add_subdirectory("${HAJINMON_PLUME_DIR}" plume EXCLUDE_FROM_ALL)

# The SDK's base GPU code (GPU registers, command stream parsing, vblank,
# interrupts, shader microcode parsing) is compiled in from the SDK sources, as
# the rexgpu-xenos plugin does.
set(SDK_GPU "${HAJINMON_REXSDK_SOURCE_DIR}/src/graphics")
set(HAJINMON_SDK_GPU_SOURCES
    ${SDK_GPU}/flags.cpp
    ${SDK_GPU}/register_file.cpp
    ${SDK_GPU}/registers.cpp
    ${SDK_GPU}/graphics_system.cpp
    ${SDK_GPU}/command_processor.cpp
    ${SDK_GPU}/packet_disassembler.cpp
    ${SDK_GPU}/sampler_info.cpp
    ${SDK_GPU}/format/ucode.cpp
    ${SDK_GPU}/pipeline/shader/shader.cpp
    ${SDK_GPU}/util/draw.cpp
    ${SDK_GPU}/util/draw_extent_estimator.cpp
)

add_library(hajinmon_plume STATIC
    src/plume_renderer.cpp
    src/plume_graphics_system.cpp
    src/plume_command_processor.cpp
    ${HAJINMON_SDK_GPU_SOURCES}
)
target_include_directories(hajinmon_plume PUBLIC src)
target_link_libraries(hajinmon_plume
    PUBLIC rex::runtime plume
    PRIVATE rex::xxhash rex::renderdoc
)
if(WIN32)
    # Plume's Direct3D 12 backend (without the Agility SDK) uses the system libraries.
    target_link_libraries(hajinmon_plume PUBLIC d3d12 dxgi dxguid)
endif()
