cmake_minimum_required(VERSION 3.24)

project(bromux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/MP>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    )
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# Coverage. Scoped to a top-level build: a consumer that add_subdirectory()s
# bromux has no business having its own build instrumented from in here.
option(BROMUX_COVERAGE "Instrument the build for gcov coverage (GCC/Clang)" OFF)
if(BROMUX_COVERAGE AND PROJECT_IS_TOP_LEVEL)
    if(MSVC)
        message(FATAL_ERROR "BROMUX_COVERAGE requires GCC or Clang (gcov instrumentation)")
    endif()
    # Atomic counters: the server loop, PTY readers and client threads hit the
    # same lines, and racing plain counters give gcov negative hit counts.
    add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
    add_link_options(--coverage)
endif()

# ---- siblings ------------------------------------------------------------------
# brolink (the transport), bropty (the terminal substrate) and, through it,
# brosearch. Each resolves through cmake/bro_deps.cmake: an existing target (a
# superbuild already added it) wins; otherwise the ../<name> working tree
# beside the top-level project; otherwise the pinned commit, fetched at
# configure. brosearch goes first, so bropty finds its target.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bro_deps.cmake)
bro_dependency(brolink GITHUB wlejon/brolink OPTIONS BROLINK_BUILD_TESTS=OFF)
bro_dependency(brosearch GITHUB wlejon/brosearch
               OPTIONS BROSEARCH_BUILD_TESTS=OFF BROSEARCH_BUILD_TOOLS=OFF)
bro_dependency(bropty GITHUB wlejon/bropty OPTIONS BROPTY_BUILD_TESTS=OFF)

# ---- library -------------------------------------------------------------------
set(BROMUX_SOURCES
    src/wire.cpp
    src/protocol.cpp
    src/protocol_input.cpp
    src/row_codec.cpp
    src/command_codec.cpp
    src/image_codec.cpp
    src/screen_model.cpp
    src/screen_source.cpp
    src/client.cpp
    src/client_requests.cpp
    src/tee.cpp
    src/server.cpp
    src/server_session.cpp
    src/server_dispatch.cpp
    src/server_frames.cpp
    src/server_images.cpp
    src/server_foreground.cpp
    src/proxy.cpp
    src/paths_common.cpp
)
# The transport (framing, the local listener and streams, spawning, the
# proxy relay) is brolink's.

add_library(bromux STATIC ${BROMUX_SOURCES})
add_library(bromux::bromux ALIAS bromux)
# MSVC writes a static library's compiler PDB beside it as <target>.pdb, which
# in Debug is the same file as the link PDB of the bromux executable
# (OUTPUT_NAME bromux below): the link fails with LNK1201.
set_target_properties(bromux PROPERTIES COMPILE_PDB_NAME bromux_lib)
target_include_directories(bromux
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bromux PUBLIC brolink bropty)
target_compile_definitions(bromux PRIVATE BROMUX_VERSION_STRING="${PROJECT_VERSION}")
if(WIN32)
    target_compile_definitions(bromux PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()
if(MSVC)
    target_compile_options(bromux PRIVATE /W4)
else()
    target_compile_options(bromux PRIVATE -Wall -Wextra)
endif()

# ---- the bromux executable: server, proxy and a small CLI ---------------------
add_executable(bromux_cli tools/bromux.cpp tools/cli_attach.cpp tools/server_images.cpp)
# ARCHIVE_OUTPUT_NAME: MSVC gives an executable that exports symbols (a
# library it links may export some) an import library named after its
# output, which would be bromux.lib beside the static library of that name.
set_target_properties(bromux_cli PROPERTIES OUTPUT_NAME bromux ARCHIVE_OUTPUT_NAME bromux_cli)
target_link_libraries(bromux_cli PRIVATE bromux)

# Compressed inline images (kitty PNG, iTerm2's formats) are decoded by
# broimage: a superbuild's target (bro has one), else the ../broimage working
# tree, else the pinned commit (with its brotensor adapter off). With
# BROMUX_WITH_BROIMAGE=OFF the server still shows raw, zlib and sixel images
# and refuses the rest.
option(BROMUX_WITH_BROIMAGE "Decode compressed inline images with broimage" ON)
if(BROMUX_WITH_BROIMAGE AND NOT TARGET broimage)
    bro_dependency(bromath GITHUB wlejon/bromath)
    # Decode only: no tensor adapter, no JIT kernels, no JavaScript API.
    bro_dependency(broimage GITHUB wlejon/broimage
                   OPTIONS BROIMAGE_WITH_TENSOR=OFF BROIMAGE_WITH_JIT=OFF BROIMAGE_ENABLE_API=OFF)
endif()
if(BROMUX_WITH_BROIMAGE AND TARGET broimage)
    target_link_libraries(bromux_cli PRIVATE broimage)
    target_compile_definitions(bromux_cli PRIVATE BROMUX_HAVE_BROIMAGE=1)
else()
    message(STATUS "bromux: broimage not found; the server refuses compressed inline images")
endif()
if(WIN32)
    target_compile_definitions(bromux_cli PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

option(BROMUX_BUILD_TESTS "Build the bromux test suite" ON)
if(BROMUX_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR BRO_BUILD_TESTS))
    include(CTest)
    add_subdirectory(tests)
endif()
