# doa4 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(doa4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(DOA4_SOURCES
    src/main.cpp
    src/cheats.cpp
    src/input_defaults.cpp
    src/crash_trace.cpp
    src/frame_capture.cpp
    src/game_fixes.cpp
    src/save_trace.cpp
    src/user_data.cpp
)

if(WIN32)
    add_executable(doa4 WIN32 ${DOA4_SOURCES})
else()
    add_executable(doa4 ${DOA4_SOURCES})
endif()

rexglue_setup_target(doa4)

# "doa4" is the internal project name; players see the game as HajinmonGame.exe
# (started by the Hajinmon.exe launcher).
set_target_properties(doa4 PROPERTIES OUTPUT_NAME HajinmonGame)

if(WIN32)
    target_link_libraries(doa4 PRIVATE dbghelp)  # crash_trace.cpp
endif()

# The GPU plugin (Xenos emulation) is loaded at runtime with --gpu_plugin=xenos
# and is looked up next to the executable: rexglue_setup_target does not copy it.
add_custom_command(TARGET doa4 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::gpu-xenos> $<TARGET_FILE_DIR:doa4>
    COMMENT "Copying the Xenos GPU plugin next to HajinmonGame.exe")
