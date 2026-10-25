# Hajinmon native renderer (experimental): renders the game with Plume
# (https://github.com/renderbag/plume) instead of the SDK's Xenos emulation.
# Built as a static library linked into the game; selected at run time with
# --renderer=plume.

set(HAJINMON_PLUME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/plume"
    CACHE PATH "Plume source tree (tools/setup_plume.cmd)")
set(HAJINMON_REXSDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/rexglue-sdk"
    CACHE PATH "ReXGlue SDK source tree")
set(HAJINMON_DXC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/dxc-bin"
    CACHE PATH "DXC binaries (https://github.com/renderbag/dxc-bin, tools/setup_plume.cmd)")

if(NOT EXISTS "${HAJINMON_PLUME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Plume not found in ${HAJINMON_PLUME_DIR}: run tools\\setup_plume.cmd (Windows) or tools/setup_plume.sh (Linux)")
endif()
if(NOT EXISTS "${HAJINMON_DXC_DIR}/inc/dxcapi.h")
    message(FATAL_ERROR "DXC not found in ${HAJINMON_DXC_DIR}: run tools\\setup_plume.cmd (Windows) or tools/setup_plume.sh (Linux)")
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
    ${SDK_GPU}/pipeline/shader/translator.cpp
    ${SDK_GPU}/pipeline/shader/translator_disasm.cpp
    ${SDK_GPU}/util/draw.cpp
    ${SDK_GPU}/shared_memory.cpp
    ${SDK_GPU}/primitive_processor.cpp
    ${SDK_GPU}/xenos.cpp
    ${SDK_GPU}/pipeline/texture/info.cpp
    ${SDK_GPU}/pipeline/texture/info_formats.cpp
    ${SDK_GPU}/pipeline/texture/util.cpp
    ${SDK_GPU}/util/draw_extent_estimator.cpp
)

add_library(hajinmon_plume STATIC
    src/plume_renderer.cpp
    src/plume_graphics_system.cpp
    src/plume_command_processor.cpp
    src/shader_catalog.cpp
    src/hlsl_translator.cpp
    src/shader_compiler.cpp
    src/shader_cache.cpp
    src/plume_shared_memory.cpp
    src/plume_primitive_processor.cpp
    src/render_target_cache.cpp
    src/texture_cache.cpp
    src/pipeline_cache.cpp
    src/png_writer.cpp
    ${HAJINMON_SDK_GPU_SOURCES}
)
target_include_directories(hajinmon_plume PUBLIC src)
target_link_libraries(hajinmon_plume
    PUBLIC rex::runtime plume
    PRIVATE rex::xxhash rex::renderdoc
)
if(WIN32)
    # Plume's Direct3D 12 backend (without the Agility SDK) uses the system libraries.
    target_link_libraries(hajinmon_plume PUBLIC d3d12 dxgi dxguid winmm)
else()
    # Plume's Vulkan backend presents to X11 windows (Xlib), dlopen loads DXC.
    find_package(X11 REQUIRED)
    target_link_libraries(hajinmon_plume PUBLIC X11::X11 ${CMAKE_DL_LIBS})
    # dxcapi.h: COM interface IDs without the MSVC __declspec(uuid) extension.
    target_compile_definitions(hajinmon_plume PUBLIC __EMULATE_UUID)
endif()

# DXC compiles the translated shaders at run time; it is loaded from the
# executable's directory.
target_include_directories(hajinmon_plume PRIVATE "${HAJINMON_DXC_DIR}/inc")
if(WIN32)
    set(HAJINMON_DXC_RUNTIME
        "${HAJINMON_DXC_DIR}/bin/x64/dxcompiler.dll"
        "${HAJINMON_DXC_DIR}/bin/x64/dxil.dll")
else()
    set(HAJINMON_DXC_RUNTIME
        "${HAJINMON_DXC_DIR}/lib/x64/libdxcompiler.so"
        "${HAJINMON_DXC_DIR}/lib/x64/libdxil.so")
endif()
add_custom_target(hajinmon_dxc_runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HAJINMON_DXC_RUNTIME} "${CMAKE_BINARY_DIR}"
    COMMENT "Copying DXC next to the executable")
add_dependencies(hajinmon_plume hajinmon_dxc_runtime)

# Offline check: translates and compiles every shader of the executable.
add_executable(hajinmon_shader_tool EXCLUDE_FROM_ALL tools/shader_tool.cpp)
target_link_libraries(hajinmon_shader_tool PRIVATE hajinmon_plume rex::xxhash)
target_include_directories(hajinmon_shader_tool PRIVATE "${HAJINMON_DXC_DIR}/inc")
set_target_properties(hajinmon_shader_tool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# The SDK's index conversion code uses SSSE3/SSE4.1 intrinsics, as in the SDK.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(hajinmon_plume PRIVATE -msse4.1)
endif()
