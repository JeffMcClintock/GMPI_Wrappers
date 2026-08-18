cmake_minimum_required(VERSION 3.30)

#set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum OS X deployment version")
#set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for Mac OS X" FORCE)

project(AU_Wrapper)

set(CMAKE_CXX_STANDARD 23)

add_definitions(-D_UNICODE)

set(srcs
AU2_Wrapper.cpp
AU2_Wrapper.h

AudioUnitView.mm

# Host-side scaffolding, shared with the VST3/CLAP/standalone wrappers - one
# list, in wrapper/CMakeLists.txt.
${GMPI_HOSTING_SRCS}

${GMPI_UI_SDK}/backends/DrawingFrameCommon.h
${GMPI_UI_SDK}/backends/DrawingFrameCommon.cpp
${GMPI_UI_SDK}/helpers/Timer.h
${GMPI_UI_SDK}/helpers/Timer.cpp
)

IF(APPLE)
set(srcs ${srcs}
${GMPI_UI_SDK}/backends/DrawingFrameMac.h
${GMPI_UI_SDK}/backends/DrawingFrameMac.mm
${GMPI_UI_SDK}/backends/CocoaGfx.h
${GMPI_UI_SDK}/backends/Gfx_base.h
${GMPI_UI_SDK}/backends/Bezier.h
)
ENDIF()

set(AU_H "${AU_SDK}/include/AudioUnitSDK/")
set(AU_S "${AU_SDK}/src/AudioUnitSDK/")

set(au_sdk_srcs
${AU_H}AUBase.h
${AU_H}AUBuffer.h
${AU_H}AUEffectBase.h
${AU_H}AUInputElement.h
${AU_H}AUMIDIBase.h
${AU_H}AUMIDIUtility.h
${AU_H}AUOutputElement.h
${AU_H}AUPlugInDispatch.h
${AU_H}AUScopeElement.h
${AU_H}AUSilentTimeout.h
${AU_H}AUUtility.h
${AU_H}AudioUnitSDK.h
${AU_H}ComponentBase.h
${AU_H}MusicDeviceBase.h
${AU_S}AUBase.cpp
${AU_S}AUBuffer.cpp
${AU_S}AUBufferAllocator.cpp
${AU_S}AUEffectBase.cpp
${AU_S}AUInputElement.cpp
${AU_S}AUMIDIBase.cpp
${AU_S}AUOutputElement.cpp
${AU_S}AUPlugInDispatch.cpp
${AU_S}AUScopeElement.cpp
${AU_S}ComponentBase.cpp
${AU_S}MusicDeviceBase.cpp
)

source_group(se_sdk FILES ${sdk_srcs})

if(APPLE)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GmpiFrameworks.cmake")
gmpi_find_frameworks(au_frameworks
    CoreFoundation
    QuartzCore
    Cocoa
    CoreText
    OpenGL
    ImageIO
    CoreGraphics
    Accelerate
    AudioUnit           # the component this wrapper implements
    AudioToolbox
)
endif()

include (GenerateExportHeader)

add_library(${PROJECT_NAME} STATIC ${sdk_srcs} ${srcs} ${au_sdk_srcs})

# Include paths ONLY for the AU wrapper library
target_include_directories(${PROJECT_NAME} PRIVATE
    ${AU_SDK_H}
    ${GMPI_SDK}
    ${GMPI_SDK}/Core
    ${GMPI_UI_SDK}
    ${GMPI_UI_SDK}/helpers
    ${ADAPTORS_COMMON_FOLDER}
)

if(APPLE)
# Plain signature, as the ten calls this replaces used: it puts the frameworks
# in this static library's link INTERFACE as well, which is how they reach the
# .component module that links AU_Wrapper.
target_link_libraries(${PROJECT_NAME} ${au_frameworks})
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE 
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Release>:NDEBUG>
  -D_UNICODE
  -DUNICODE
)

if(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE TRUE)
  set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE_EXTENSION component)

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

set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".au")
SET_TARGET_PROPERTIES(${PROJECT_NAME} PROPERTIES FOLDER "wrappers")

