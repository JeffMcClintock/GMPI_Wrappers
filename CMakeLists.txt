# This cmake does not build on it's own, it's meant to be consumed by other projects, e.g. GMPI_Plugins repo

cmake_minimum_required(VERSION 3.30)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # iOS 15 introduced AUAudioUnit's MIDI 2.0 protocol property, which the
    # AU3 wrapper reports. x86_64 must not be forced here: device builds are
    # arm64-only, and the simulator picks its own slice.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS deployment version")
else()
# macOS 12 SDK introduced MIDI 2.0
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum MacOS deployment version")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for MacOS" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)

include_directories(
${CMAKE_CURRENT_SOURCE_DIR}
)

add_subdirectory(wrapper)

