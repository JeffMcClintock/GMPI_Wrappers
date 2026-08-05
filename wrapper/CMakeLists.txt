cmake_minimum_required(VERSION 3.30)

set(ADAPTORS_COMMON_FOLDER ${CMAKE_CURRENT_SOURCE_DIR}/common)

add_subdirectory(VST3)
add_subdirectory(CLAP)

if(APPLE)
add_subdirectory(AU2)
endif()

# Off by default - consumers of this directory should not grow a test binary
# unless they ask for it.
option(GMPI_WRAPPERS_BUILD_TESTS "Build the wrappers' host-side tests" OFF)
if(GMPI_WRAPPERS_BUILD_TESTS AND UNIX AND NOT APPLE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests gmpi_wrapper_tests)
endif()

