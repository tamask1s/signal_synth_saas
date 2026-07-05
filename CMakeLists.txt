cmake_minimum_required(VERSION 3.16)

project(
    signal_synth_saas
    VERSION 0.1.0
    DESCRIPTION "Apache orchestration layer for Synsigra challenge packages"
    LANGUAGES CXX
)

include(CTest)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(
    SIGNAL_SYNTH_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../signal_synth"
    CACHE PATH
    "Path to the sibling signal_synth checkout"
)

if(NOT EXISTS "${SIGNAL_SYNTH_ROOT}/CMakeLists.txt")
    message(
        WARNING
        "signal_synth was not found at ${SIGNAL_SYNTH_ROOT}. "
        "The Apache health module can still be built, but later worker tasks "
        "require the sibling checkout."
    )
endif()

find_package(Git QUIET)
set(SYN_SIG_RA_GIT_COMMIT "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE SYN_SIG_RA_GIT_RESULT
        OUTPUT_VARIABLE SYN_SIG_RA_GIT_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(SYN_SIG_RA_GIT_RESULT EQUAL 0 AND NOT SYN_SIG_RA_GIT_OUTPUT STREQUAL "")
        set(SYN_SIG_RA_GIT_COMMIT "${SYN_SIG_RA_GIT_OUTPUT}")
    endif()
endif()

configure_file(
    include/syn_sig_ra/build_info.h.in
    generated/syn_sig_ra/build_info.h
    @ONLY
)

add_library(syn_sig_ra_core STATIC
    src/api/route.cpp
)
set_target_properties(syn_sig_ra_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(syn_sig_ra_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

find_program(APXS_EXECUTABLE NAMES apxs apxs2 REQUIRED)
execute_process(
    COMMAND "${APXS_EXECUTABLE}" -q INCLUDEDIR
    RESULT_VARIABLE APXS_INCLUDE_RESULT
    OUTPUT_VARIABLE APACHE_INCLUDE_DIR
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT APXS_INCLUDE_RESULT EQUAL 0 OR APACHE_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR "Unable to query the Apache include directory with apxs")
endif()

execute_process(
    COMMAND "${APXS_EXECUTABLE}" -q APR_CONFIG
    RESULT_VARIABLE APXS_APR_RESULT
    OUTPUT_VARIABLE APR_CONFIG_EXECUTABLE
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT APXS_APR_RESULT EQUAL 0 OR NOT EXISTS "${APR_CONFIG_EXECUTABLE}")
    find_program(APR_CONFIG_EXECUTABLE NAMES apr-1-config REQUIRED)
endif()

execute_process(
    COMMAND "${APR_CONFIG_EXECUTABLE}" --includedir
    RESULT_VARIABLE APR_INCLUDE_RESULT
    OUTPUT_VARIABLE APR_INCLUDE_DIR
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT APR_INCLUDE_RESULT EQUAL 0 OR APR_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR "Unable to query the APR include directory")
endif()

add_library(mod_syn_sig_ra MODULE
    src/mod_syn_sig_ra.cpp
)
set_target_properties(mod_syn_sig_ra PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mod_syn_sig_ra"
)
target_include_directories(mod_syn_sig_ra
    PRIVATE
        "${APACHE_INCLUDE_DIR}"
        "${APR_INCLUDE_DIR}"
)
target_link_libraries(mod_syn_sig_ra PRIVATE syn_sig_ra_core)

install(
    TARGETS mod_syn_sig_ra
    LIBRARY DESTINATION lib/apache2/modules
)

if(BUILD_TESTING)
    add_executable(syn_sig_ra_route_test
        test/unit/route_test.cpp
    )
    target_link_libraries(syn_sig_ra_route_test PRIVATE syn_sig_ra_core)

    add_test(NAME route_unit COMMAND syn_sig_ra_route_test)

    find_program(NM_EXECUTABLE NAMES nm llvm-nm)
    if(NM_EXECUTABLE)
        add_test(
            NAME module_registration_symbol
            COMMAND "${CMAKE_COMMAND}"
                "-DNM_EXECUTABLE=${NM_EXECUTABLE}"
                "-DMODULE_FILE=$<TARGET_FILE:mod_syn_sig_ra>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/test/module_symbol_test.cmake"
        )
    endif()
endif()
