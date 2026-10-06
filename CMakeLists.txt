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
# bropty (the terminal substrate) and, through it, brosearch. Each resolves
# like bro's siblings: an existing target (a superbuild already added it)
# wins; otherwise the standalone repo beside the top-level project
# (../<name>, overridable with -D<NAME>_DIR=<path>); otherwise the
# third_party/<name> submodule of the top-level project. The fallback is flat:
# a consumer vendoring bromux carries bropty and brosearch side by side in its
# own third_party/. brosearch goes first, so bropty finds its target.
function(bromux_sibling name what)
    if(TARGET ${name})
        return()
    endif()
    string(TOUPPER "${name}" _upper)
    set(${_upper}_DIR "${CMAKE_SOURCE_DIR}/../${name}" CACHE PATH
        "Path to the standalone ${name} repo (set to a missing path to force the submodule)")
    if(EXISTS "${${_upper}_DIR}/CMakeLists.txt")
        message(STATUS "bromux: ${name} from ${${_upper}_DIR}")
        add_subdirectory("${${_upper}_DIR}" "${CMAKE_BINARY_DIR}/${name}" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/${name}/CMakeLists.txt")
        message(STATUS "bromux: ${name} from ${CMAKE_SOURCE_DIR}/third_party/${name} (submodule)")
        add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/${name}" "${CMAKE_BINARY_DIR}/${name}" EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR
            "bromux needs ${name} (${what}): not found at ${${_upper}_DIR} or "
            "${CMAKE_SOURCE_DIR}/third_party/${name}. Clone https://github.com/wlejon/${name} next "
            "to this repo, run `git submodule update --init --recursive`, or pass -D${_upper}_DIR=<path>.")
    endif()
endfunction()

set(BROSEARCH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BROSEARCH_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
bromux_sibling(brosearch "bropty's regex scrollback search")
set(BROPTY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
bromux_sibling(bropty "the PTY and the terminal emulator")

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
if(WIN32)
    list(APPEND BROMUX_SOURCES
        src/win/paths_win.cpp
        src/win/stream_win.cpp
        src/win/spawn_win.cpp
        src/win/loop_win.cpp
    )
else()
    list(APPEND BROMUX_SOURCES
        src/posix/paths_posix.cpp
        src/posix/stream_posix.cpp
        src/posix/spawn_posix.cpp
        src/posix/loop_posix.cpp
    )
endif()

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
target_link_libraries(bromux PUBLIC bropty)
target_compile_definitions(bromux PRIVATE BROMUX_VERSION_STRING="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(bromux PRIVATE advapi32)
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
# broimage when it is around: a superbuild's target (bro has one), else the
# standalone repo beside this one (with its brotensor adapter off). Without
# it the server still shows raw, zlib and sixel images and refuses the rest.
option(BROMUX_WITH_BROIMAGE "Decode compressed inline images with broimage (when found)" ON)
if(BROMUX_WITH_BROIMAGE AND NOT TARGET broimage)
    set(BROIMAGE_DIR "${CMAKE_SOURCE_DIR}/../broimage" CACHE PATH "Path to the standalone broimage repo")
    set(BROMATH_DIR "${CMAKE_SOURCE_DIR}/../bromath" CACHE PATH "Path to the standalone bromath repo")
    if(EXISTS "${BROIMAGE_DIR}/CMakeLists.txt" AND EXISTS "${BROMATH_DIR}/CMakeLists.txt")
        message(STATUS "bromux: broimage from ${BROIMAGE_DIR}")
        # Decode only: no tensor adapter, no JIT kernels, no JavaScript API.
        set(BROIMAGE_WITH_TENSOR OFF CACHE BOOL "" FORCE)
        set(BROIMAGE_WITH_JIT OFF CACHE BOOL "" FORCE)
        set(BROIMAGE_ENABLE_API OFF CACHE BOOL "" FORCE)
        add_subdirectory("${BROIMAGE_DIR}" "${CMAKE_BINARY_DIR}/broimage" EXCLUDE_FROM_ALL)
    endif()
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
