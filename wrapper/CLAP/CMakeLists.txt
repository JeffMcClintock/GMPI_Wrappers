cmake_minimum_required(VERSION 3.30)

project(CLAP_Wrapper)

set(CMAKE_CXX_STANDARD 20)

add_definitions(-D_UNICODE)

if (MSVC)
    # Floating Point Model: Fast (/fp:fast)
    # Buffer Security Check: No (/GS-)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /fp:fast /GS-")
endif()

set(srcs
# Host-side scaffolding, shared with the VST3/AU/standalone wrappers - one list,
# in wrapper/CMakeLists.txt.
${GMPI_HOSTING_SRCS}

Factory_CLAP.h
Factory_CLAP.cpp
Processor_CLAP.h
Processor_CLAP.cpp
Controller_CLAP.h
Controller_CLAP.cpp
Editor_CLAP.h
Editor_CLAP.cpp

${GMPI_UI_SDK}/backends/DrawingFrameCommon.h
${GMPI_UI_SDK}/backends/DrawingFrameCommon.cpp
${GMPI_UI_SDK}/backends/DrawingFrameWin.h
${GMPI_UI_SDK}/backends/DrawingFrameWin.cpp
${GMPI_UI_SDK}/backends/DirectXGfx.h
${GMPI_UI_SDK}/backends/DirectXGfx.cpp
${GMPI_UI_SDK}/helpers/Timer.h
${GMPI_UI_SDK}/helpers/Timer.cpp
)

IF(WIN32)
set(srcs ${srcs}
#SEVSTGUIEditorWin.h
#SEVSTGUIEditorWin.cpp
#${VST3_SDK}/public.sdk/source/main/dllmain.cpp
)
ENDIF()

if(APPLE)
set(srcs ${srcs}
#SEVSTGUIEditorMac.h
#SEVSTGUIEditorMac.cpp
${GMPI_UI_SDK}/backends/DrawingFrameMac.mm
${GMPI_UI_SDK}/backends/CocoaGfx.h
${GMPI_UI_SDK}/backends/Gfx_base.h
${GMPI_UI_SDK}/backends/Bezier.h
#${VST3_SDK}/public.sdk/source/main/macmain.cpp
)
ENDIF()

include_directories(
${CLAP_SDK}/include
${CLAP_HELPERS_SDK}/include
${GMPI_SDK}
${GMPI_SDK}/Core
${GMPI_UI_SDK}
${GMPI_UI_SDK}/helpers
${ADAPTORS_COMMON_FOLDER}
)

source_group(se_sdk FILES ${sdk_srcs})

if(APPLE)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GmpiFrameworks.cmake")
gmpi_find_frameworks(clap_frameworks
    CoreFoundation
    QuartzCore
    Cocoa
    CoreText
    OpenGL
    ImageIO
    CoreGraphics
    Accelerate
)

# gmpi_ui's Mac backend compiles MacFileDialog, whose UTType path must be
# linked weakly - see gmpi_weak_frameworks() for why. A static library, so an
# omission here surfaces only when a plugin module links it.
gmpi_weak_frameworks(clap_weak_frameworks)
list(APPEND clap_frameworks ${clap_weak_frameworks})
endif()

include (GenerateExportHeader)

add_library(${PROJECT_NAME} STATIC ${sdk_srcs} ${srcs} ${vst_gui_minimal_srcs})

target_compile_definitions(${PROJECT_NAME} PRIVATE 
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Release>:NDEBUG>
)

if(APPLE)
# Plain signature, like the Windows link below - CMake refuses to mix the plain
# and keyword forms on one target.
target_link_libraries(${PROJECT_NAME} ${clap_frameworks})
endif()

if(CMAKE_HOST_WIN32)
target_link_libraries(${PROJECT_NAME} Dwmapi dxguid d3d11 d2d1 dwrite windowscodecs)
endif()

target_compile_definitions (${PROJECT_NAME} PRIVATE -D_UNICODE -DUNICODE)

if(APPLE)
set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE TRUE)
set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE_EXTENSION CLAP)
endif()

# required by Linux build: this static lib gets linked into the .clap shared
# module, so its objects must be position-independent (same as VST3_Wrapper).
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

SET_TARGET_PROPERTIES(${PROJECT_NAME} PROPERTIES FOLDER "wrappers")
