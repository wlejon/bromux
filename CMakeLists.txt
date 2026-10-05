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

# ---- bropty ------------------------------------------------------------------
# The terminal substrate. Resolved like bro's siblings: an existing `bropty`
# target (a superbuild already added it) wins; otherwise the standalone repo
# beside this one (../bropty), overridable with -DBROPTY_DIR=<path>.
if(NOT TARGET bropty)
    get_filename_component(_bropty_default "${CMAKE_CURRENT_SOURCE_DIR}/../bropty" ABSOLUTE)
    set(BROPTY_DIR "${_bropty_default}" CACHE PATH "Path to the bropty repo")
    if(NOT EXISTS "${BROPTY_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "bromux needs bropty: clone it beside bromux (../bropty) or pass -DBROPTY_DIR=<path>")
    endif()
    message(STATUS "bromux: bropty from ${BROPTY_DIR}")
    set(BROPTY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${BROPTY_DIR}" "${CMAKE_BINARY_DIR}/bropty" EXCLUDE_FROM_ALL)
endif()

# ---- library -------------------------------------------------------------------
set(BROMUX_SOURCES
    src/wire.cpp
    src/protocol.cpp
    src/protocol_input.cpp
    src/row_codec.cpp
    src/screen_model.cpp
    src/client.cpp
    src/client_requests.cpp
    src/tee.cpp
    src/server.cpp
    src/server_session.cpp
    src/server_dispatch.cpp
    src/server_frames.cpp
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
add_executable(bromux_cli tools/bromux.cpp tools/cli_attach.cpp)
set_target_properties(bromux_cli PROPERTIES OUTPUT_NAME bromux)
target_link_libraries(bromux_cli PRIVATE bromux)
if(WIN32)
    target_compile_definitions(bromux_cli PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

option(BROMUX_BUILD_TESTS "Build the bromux test suite" ON)
if(BROMUX_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR BRO_BUILD_TESTS))
    include(CTest)
    add_subdirectory(tests)
endif()
