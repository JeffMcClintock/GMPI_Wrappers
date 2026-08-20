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
# Universal stays the DEFAULT. No longer FORCEd (2026-08-21): a consumer that
# has already chosen keeps its choice -- TIDE Rack chooses arm64, and with the
# FORCE in place even a command-line -D was silently overwritten. Every other
# consumer sets nothing and gets the same universal build as before.
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for MacOS")
endif()
endif()
set(CMAKE_CXX_STANDARD 17)

include_directories(
${CMAKE_CURRENT_SOURCE_DIR}
)

add_subdirectory(wrapper)

