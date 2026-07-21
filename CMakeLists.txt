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

option(
    SYN_SIG_RA_ENABLE_INTEGRATION_TESTS
    "Enable Apache-backed end-to-end integration tests"
    OFF
)

find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_path(JANSSON_INCLUDE_DIR jansson.h REQUIRED)
find_library(JANSSON_LIBRARY NAMES jansson REQUIRED)
add_library(Jansson::Jansson UNKNOWN IMPORTED)
set_target_properties(Jansson::Jansson PROPERTIES
    IMPORTED_LOCATION "${JANSSON_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${JANSSON_INCLUDE_DIR}"
)

set(
    SIGNAL_SYNTH_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../signal_synth"
    CACHE PATH
    "Path to the sibling signal_synth checkout"
)

if(NOT EXISTS "${SIGNAL_SYNTH_ROOT}/CMakeLists.txt")
    message(
        FATAL_ERROR
        "signal_synth was not found at ${SIGNAL_SYNTH_ROOT}. "
        "This project requires the sibling checkout and does not vendor it."
    )
endif()

set(
    SYN_SIG_RA_DEFAULT_DATA_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/var"
)
set(
    SYN_SIG_RA_DEFAULT_SIGNAL_SYNTH_CLI
    "${SIGNAL_SYNTH_ROOT}/build/signal-synth"
)
set(
    SYN_SIG_RA_DEFAULT_PACK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/packs"
)
set(SYN_SIG_RA_DEFAULT_PUBLIC_BASE_PATH "/syn_sig_ra")

file(MAKE_DIRECTORY
    "${SYN_SIG_RA_DEFAULT_DATA_ROOT}/work"
    "${SYN_SIG_RA_DEFAULT_DATA_ROOT}/packages"
)

find_package(Git QUIET)
set(
    SYN_SIG_RA_EXPECTED_SIGNAL_SYNTH_COMMIT
    "2531c5c21a1917f9704fa9562d0a32ebacc821da"
    CACHE STRING
    "Exact signal_synth commit accepted by this SaaS release"
    FORCE
)
if(NOT Git_FOUND)
    message(FATAL_ERROR "Git is required to verify the pinned signal_synth producer")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${SIGNAL_SYNTH_ROOT}"
    RESULT_VARIABLE SIGNAL_SYNTH_GIT_RESULT
    OUTPUT_VARIABLE SIGNAL_SYNTH_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain
    WORKING_DIRECTORY "${SIGNAL_SYNTH_ROOT}"
    OUTPUT_VARIABLE SIGNAL_SYNTH_GIT_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT SIGNAL_SYNTH_GIT_RESULT EQUAL 0 OR
   NOT SIGNAL_SYNTH_GIT_COMMIT STREQUAL SYN_SIG_RA_EXPECTED_SIGNAL_SYNTH_COMMIT OR
   NOT SIGNAL_SYNTH_GIT_DIRTY STREQUAL "")
    message(FATAL_ERROR
        "signal_synth must be the clean pinned producer "
        "${SYN_SIG_RA_EXPECTED_SIGNAL_SYNTH_COMMIT}; found ${SIGNAL_SYNTH_GIT_COMMIT}")
endif()

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

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/doc/openapi.yaml" SYN_SIG_RA_OPENAPI_YAML)
configure_file(
    include/syn_sig_ra/openapi_document.h.in
    generated/syn_sig_ra/openapi_document.h
    @ONLY
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/viewer/index.html" SYN_SIG_RA_VIEWER_HTML)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/viewer/style.css" SYN_SIG_RA_VIEWER_CSS)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/viewer/signal-viewer.js" SYN_SIG_RA_SIGNAL_VIEWER_JS)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/viewer/app.js" SYN_SIG_RA_VIEWER_APP_JS)
configure_file(
    include/syn_sig_ra/viewer_assets.h.in
    generated/syn_sig_ra/viewer_assets.h
    @ONLY
)

set(SIGNAL_SYNTH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SIGNAL_SYNTH_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(SIGNAL_SYNTH_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${SIGNAL_SYNTH_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/signal_synth_dependency"
    EXCLUDE_FROM_ALL
)
set_target_properties(signal_synth PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(syn_sig_ra_core STATIC
    src/artifact/artifact_store.cpp
    src/auth/api_key_auth.cpp
    src/auth/password_auth.cpp
    src/auth/sha256.cpp
    src/catalog/pack_catalog.cpp
    src/config/runtime_config.cpp
    src/email/transactional_email.cpp
    src/job/job_request.cpp
    src/job/random_id.cpp
    src/integration/core_contract.cpp
    src/storage/metadata_store.cpp
    src/viewer/signal_viewer.cpp
    src/worker/worker.cpp
    src/api/mcp_server.cpp
    src/api/route.cpp
)
set_target_properties(syn_sig_ra_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(syn_sig_ra_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PRIVATE
        "${SIGNAL_SYNTH_ROOT}/src"
)
target_link_libraries(syn_sig_ra_core
    PUBLIC
        OpenSSL::Crypto
        SQLite::SQLite3
        CURL::libcurl
        Jansson::Jansson
        signal_synth::signal_synth
)

add_executable(syn_sig_ra_admin
    src/admin/main.cpp
)
target_link_libraries(syn_sig_ra_admin PRIVATE syn_sig_ra_core)

add_executable(syn_sig_ra_worker
    src/worker/main.cpp
)
target_link_libraries(syn_sig_ra_worker PRIVATE syn_sig_ra_core)

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
install(
    TARGETS syn_sig_ra_admin
    RUNTIME DESTINATION bin
)
install(
    TARGETS syn_sig_ra_worker
    RUNTIME DESTINATION bin
)

if(BUILD_TESTING)
    add_executable(syn_sig_ra_route_test
        test/unit/route_test.cpp
    )
    target_link_libraries(syn_sig_ra_route_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_config_test
        test/unit/runtime_config_test.cpp
    )
    target_link_libraries(syn_sig_ra_config_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_auth_test
        test/unit/api_key_auth_test.cpp
    )
    target_link_libraries(syn_sig_ra_auth_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_pack_catalog_test
        test/unit/pack_catalog_test.cpp
    )
    target_link_libraries(syn_sig_ra_pack_catalog_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_job_test
        test/unit/job_test.cpp
    )
    target_link_libraries(syn_sig_ra_job_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_mcp_server_test
        test/unit/mcp_server_test.cpp
    )
    target_link_libraries(syn_sig_ra_mcp_server_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_worker_test
        test/unit/worker_test.cpp
    )
    target_link_libraries(syn_sig_ra_worker_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_core_contract_test
        test/unit/core_contract_test.cpp
    )
    target_link_libraries(syn_sig_ra_core_contract_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_metadata_schema_test
        test/unit/metadata_schema_test.cpp
    )
    target_link_libraries(syn_sig_ra_metadata_schema_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_signal_viewer_test
        test/unit/signal_viewer_test.cpp
    )
    target_link_libraries(syn_sig_ra_signal_viewer_test PRIVATE syn_sig_ra_core)

    add_executable(syn_sig_ra_artifact_delivery_test
        test/unit/artifact_delivery_test.cpp
    )
    target_link_libraries(
        syn_sig_ra_artifact_delivery_test PRIVATE syn_sig_ra_core)

    add_test(NAME route_unit COMMAND syn_sig_ra_route_test)
    add_test(NAME runtime_config_unit COMMAND syn_sig_ra_config_test)
    add_test(NAME api_key_auth_unit COMMAND syn_sig_ra_auth_test)
    add_test(NAME pack_catalog_unit COMMAND syn_sig_ra_pack_catalog_test)
    add_test(NAME job_unit COMMAND syn_sig_ra_job_test)
    add_test(NAME mcp_server_unit COMMAND syn_sig_ra_mcp_server_test)
    add_test(NAME worker_unit COMMAND syn_sig_ra_worker_test)
    add_test(NAME core_contract_unit COMMAND syn_sig_ra_core_contract_test)
    add_test(NAME metadata_schema_unit COMMAND syn_sig_ra_metadata_schema_test)
    add_test(NAME signal_viewer_unit COMMAND syn_sig_ra_signal_viewer_test)
    add_test(
        NAME artifact_delivery_unit COMMAND syn_sig_ra_artifact_delivery_test)
    find_program(NODE_EXECUTABLE NAMES node nodejs)
    if(NODE_EXECUTABLE)
        add_test(
            NAME viewer_client_unit
            COMMAND "${NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/test/unit/viewer_client_test.js"
        )
    endif()
    add_test(
        NAME user_facing_brand
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test/user_facing_brand_test.sh"
    )

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

    if(SYN_SIG_RA_ENABLE_INTEGRATION_TESTS)
        add_test(
            NAME integration_e2e_smoke
            COMMAND
                "${CMAKE_CURRENT_SOURCE_DIR}/test/integration/e2e_smoke.sh"
        )
        set_tests_properties(
            integration_e2e_smoke
            PROPERTIES
                ENVIRONMENT
                    "SYN_SIG_RA_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};SIGNAL_SYNTH_ROOT=${SIGNAL_SYNTH_ROOT};APXS_EXECUTABLE=${APXS_EXECUTABLE};SYN_SIG_RA_MODULE=$<TARGET_FILE:mod_syn_sig_ra>;SYN_SIG_RA_ADMIN=$<TARGET_FILE:syn_sig_ra_admin>;SYN_SIG_RA_WORKER=$<TARGET_FILE:syn_sig_ra_worker>"
        )
    endif()
endif()
