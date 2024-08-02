cmake_minimum_required(VERSION 3.15)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum MacOS deployment version")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for MacOS" FORCE)
set(CMAKE_CXX_STANDARD 17)

include_directories(
${CMAKE_CURRENT_SOURCE_DIR}
)

add_subdirectory(wrapper)

