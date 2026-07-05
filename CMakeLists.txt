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
