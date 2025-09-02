cmake_minimum_required(VERSION 3.15)

set(ADAPTORS_COMMON_FOLDER ${CMAKE_CURRENT_SOURCE_DIR}/common)

# add_subdirectory(plist_util)

add_subdirectory(VST3)

if(APPLE)
add_subdirectory(AU2)
endif()

