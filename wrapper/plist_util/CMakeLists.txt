cmake_minimum_required(VERSION 3.15)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum OS X deployment version")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for Mac OS X" FORCE)

project(plist_util)

set(CMAKE_CXX_STANDARD 20)

set(srcs
plist_util.cpp
${ADAPTORS_COMMON_FOLDER}/dynamic_linking.h
${ADAPTORS_COMMON_FOLDER}/dynamic_linking.cpp
${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.h
${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.cpp
)

include_directories(
${gmpi_sdk_folder}/Core
${ADAPTORS_COMMON_FOLDER}
)

# Build the Info.plist generator
add_executable(plist_util ${srcs})

target_compile_definitions(${PROJECT_NAME} PRIVATE 
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Release>:NDEBUG>
  -D_UNICODE
  -DUNICODE
)

if(APPLE)
  # generate debug symbols
  target_compile_options(${PROJECT_NAME} PRIVATE
      $<$<CONFIG:Debug>:-g>
      -Wno-deprecated-declarations
      -Wno-deprecated-enum-enum-conversion
)
  
  set_target_properties(${PROJECT_NAME} PROPERTIES XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT
    $<$<CONFIG:Debug>:dwarf-with-dsym>
  )

endif()
