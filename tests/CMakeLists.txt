enable_testing()

# Each test is a plain executable whose exit status is non-zero on any failed
# check (check.h); none of them rely on assert().
#   bromux_test(<name> [ARGS <test arguments...>])
function(bromux_test name)
    cmake_parse_arguments(PARSE_ARGV 1 BT "" "TIMEOUT" "ARGS")
    add_executable(${name} ${name}.cpp ${BT_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE bromux)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name} ${BT_ARGS})
    if(NOT BT_TIMEOUT)
        set(BT_TIMEOUT 600)
    endif()
    set_tests_properties(${name} PROPERTIES TIMEOUT ${BT_TIMEOUT} SKIP_RETURN_CODE 77
                         FIXTURES_REQUIRED process_census)
endfunction()

# Process leak census around the suite: no test may leave helper programs,
# servers or (Windows) orphaned console hosts behind. ctest pulls these in
# even when only some tests are selected.
add_executable(proc_census proc_census.cpp)
target_include_directories(proc_census PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set(BROMUX_CENSUS_FILE ${CMAKE_CURRENT_BINARY_DIR}/process_census.txt)
add_test(NAME process_census_before COMMAND proc_census baseline ${BROMUX_CENSUS_FILE})
add_test(NAME process_census_after COMMAND proc_census check ${BROMUX_CENSUS_FILE})
set_tests_properties(process_census_before PROPERTIES FIXTURES_SETUP process_census)
set_tests_properties(process_census_after PROPERTIES FIXTURES_CLEANUP process_census TIMEOUT 60)

bromux_test(test_wire)
bromux_test(test_codec)
bromux_test(test_source)

# Helpers the end-to-end tests run: a session program that behaves the same
# on every platform, and a client process to kill mid-stream.
add_executable(mux_child mux_child.cpp)
target_include_directories(mux_child PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

bromux_test(test_server ARGS $<TARGET_FILE:mux_child>)
add_dependencies(test_server mux_child)
bromux_test(test_oracle ARGS $<TARGET_FILE:mux_child> TIMEOUT 1200)
add_dependencies(test_oracle mux_child)

add_executable(mux_client_child mux_client_child.cpp)
target_link_libraries(mux_client_child PRIVATE bromux)
bromux_test(test_flow ARGS $<TARGET_FILE:mux_child> $<TARGET_FILE:mux_client_child> TIMEOUT 1200)
add_dependencies(test_flow mux_child mux_client_child)

bromux_test(test_daemon ARGS $<TARGET_FILE:bromux_cli> $<TARGET_FILE:mux_child>)
add_dependencies(test_daemon mux_child bromux_cli)

# Remote attach over the real ssh: skipped unless BROMUX_TEST_SSH* name a
# remote with bromux and mux_child built (see the file).
bromux_test(test_remote TIMEOUT 1800)
