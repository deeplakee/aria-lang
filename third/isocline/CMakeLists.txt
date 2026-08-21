# -----------------------------------------------------------------------------
# Isocline - A portable readline alternative
# Copyright (c) 2021, Daan Leijen
# MIT License
#
# This CMakeLists.txt wraps isocline as a third-party module for C++ projects.
# Usage in your project:
#   add_subdirectory(isocline)
#   target_link_libraries(your_target PRIVATE isocline)
# -----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.10)
project(isocline C)

set(IC_SOURCES
    src/isocline.c
)

# When IC_SEPARATE_OBJS is defined, compile each .c file separately.
# By default all sources are amalgamated via #include in isocline.c,
# which is cleaner and avoids exporting internal symbols.
option(IC_SEPARATE_OBJS "Compile with separate object files" OFF)

if(IC_SEPARATE_OBJS)
    set(IC_SOURCES
        src/attr.c
        src/bbcode.c
        src/common.c
        src/completions.c
        src/completers.c
        src/editline.c
        src/highlight.c
        src/history.c
        src/stringbuf.c
        src/term.c
        src/tty_esc.c
        src/tty.c
        src/undo.c
    )
endif()

# -----------------------------------------------------------------------------
# Static library
# -----------------------------------------------------------------------------
add_library(isocline STATIC ${IC_SOURCES})
target_include_directories(isocline PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_definitions(isocline PRIVATE
    $<$<NOT:$<BOOL:${IC_SEPARATE_OBJS}>>:_CRT_NONSTDC_NO_WARNINGS>
    $<$<NOT:$<BOOL:${IC_SEPARATE_OBJS}>>:_CRT_SECURE_NO_WARNINGS>
)

# On Windows, link against the console API
if(WIN32)
    target_compile_definitions(isocline PRIVATE _CRT_NONSTDC_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
endif()

# Enable PIC for shared library consumers
set_property(TARGET isocline PROPERTY POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
# IDE folder
# -----------------------------------------------------------------------------
set_target_properties(isocline PROPERTIES FOLDER "third_party")
