cmake_minimum_required(VERSION 3.30)

# AudioUnit V3 wrapper - a static library for macOS AND iOS, consumed by an
# app-extension (.appex) target in the plugin project, which also compiles
# wrapperAu3.mm (see that file for why) and uses appex-Info.plist.in.
#
# Unlike AU2 this needs no AudioUnitSDK checkout: AUAudioUnit is AudioToolbox
# API, straight from the platform SDK.

project(AU3_Wrapper)

set(CMAKE_CXX_STANDARD 23)

add_definitions(-D_UNICODE)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(GMPI_AU3_IOS TRUE)
endif()

set(srcs
AU3_Wrapper.h
AU3_Wrapper.mm
AU3_ViewController.h
AU3_ViewController.mm

# Host-side scaffolding, shared with the VST3/CLAP/AU2/standalone wrappers -
# one list, in wrapper/CMakeLists.txt.
${GMPI_HOSTING_SRCS}

${GMPI_UI_SDK}/backends/DrawingFrameCommon.h
${GMPI_UI_SDK}/backends/DrawingFrameCommon.cpp
${GMPI_UI_SDK}/helpers/Timer.h
${GMPI_UI_SDK}/helpers/Timer.cpp
)

if(GMPI_AU3_IOS)
set(srcs ${srcs}
${GMPI_UI_SDK}/backends/DrawingFrameIos.h
${GMPI_UI_SDK}/backends/DrawingFrameIos.mm
${GMPI_UI_SDK}/backends/CocoaGfx.h
${GMPI_UI_SDK}/backends/Gfx_base.h
${GMPI_UI_SDK}/backends/Bezier.h
)
else()
set(srcs ${srcs}
${GMPI_UI_SDK}/backends/DrawingFrameMac.h
${GMPI_UI_SDK}/backends/DrawingFrameMac.mm
${GMPI_UI_SDK}/backends/CocoaGfx.h
${GMPI_UI_SDK}/backends/Gfx_base.h
${GMPI_UI_SDK}/backends/Bezier.h
)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GmpiFrameworks.cmake")

# The frameworks both platforms share. AUViewController lives in CoreAudioKit;
# AVAudioFormat in AVFAudio; MIDIEventList walking in CoreMIDI.
set(au3_framework_names
    CoreFoundation
    QuartzCore
    CoreText
    ImageIO
    CoreGraphics
    Accelerate
    AudioToolbox
    AVFAudio
    CoreAudioKit
    CoreMIDI
)

if(GMPI_AU3_IOS)
    list(APPEND au3_framework_names UIKit Foundation)
else()
    list(APPEND au3_framework_names Cocoa AudioUnit)
endif()

gmpi_find_frameworks(au3_frameworks ${au3_framework_names})

if(NOT GMPI_AU3_IOS)
    # gmpi_ui's Mac backend compiles MacFileDialog, whose UTType path must be
    # linked weakly - see gmpi_weak_frameworks() for why. The iOS frame has no
    # file dialog, so this is macOS-only.
    gmpi_weak_frameworks(au3_weak_frameworks)
    list(APPEND au3_frameworks ${au3_weak_frameworks})
endif()

add_library(${PROJECT_NAME} STATIC ${srcs})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${GMPI_SDK}
    ${GMPI_SDK}/Core
    ${GMPI_UI_SDK}
    ${GMPI_UI_SDK}/helpers
    ${ADAPTORS_COMMON_FOLDER}
)

# Plain signature, matching AU2: the frameworks land in this static library's
# link INTERFACE, which is how they reach the .appex that links AU3_Wrapper.
target_link_libraries(${PROJECT_NAME} ${au3_frameworks})

target_compile_definitions(${PROJECT_NAME} PRIVATE
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Release>:NDEBUG>
  -D_UNICODE
  -DUNICODE
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:-g>
    -Wno-deprecated-declarations
    -Wno-deprecated-enum-enum-conversion
)

set_target_properties(${PROJECT_NAME} PROPERTIES XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT
  $<$<CONFIG:Debug>:dwarf-with-dsym>
)

SET_TARGET_PROPERTIES(${PROJECT_NAME} PROPERTIES FOLDER "wrappers")
