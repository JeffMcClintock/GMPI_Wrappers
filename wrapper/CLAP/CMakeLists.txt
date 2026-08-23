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

# Linux: the X11 editor. BACKLOG S43(ii), ruled in by Jeff -- "if CLAP wrapper
# lacks GUI support, we need to add it".
#
# This mirrors wrapper/VST3's X11 arm deliberately, source list and all: that
# wrapper has shipped a working Linux editor for longer, and copying it verbatim
# is worth more than improving it in passing. The CPU renderer is the same stack
# -- gmpi_ui's X11 backend brings no font, shaping or image-decode code of its
# own, so a consumer must supply CpuTextEngine, FontProvider and DecodeImage or
# geometry draws and text silently does not.
#
# X11 rather than Wayland, on Jeff's reading that DAWs support only X11 -- which
# the VST3 wrapper's own CMake independently records, calling X11 "the only
# Linux embedding" before VST3 3.8.0 and falling back to it whenever Wayland is
# unavailable. CLAP defines CLAP_WINDOW_API_WAYLAND too; nothing here uses it
# yet, and that is a separate decision rather than an oversight.
if(UNIX AND NOT APPLE)
set(srcs ${srcs}
${GMPI_UI_SDK}/backends/DrawingFrameX11.h
${GMPI_UI_SDK}/backends/DrawingFrameX11.cpp
${GMPI_UI_SDK}/backends/CpuGfx.h
${GMPI_UI_SDK}/backends/CpuEncode.h
${GMPI_UI_SDK}/helpers/CpuTextEngine.h
${GMPI_UI_SDK}/helpers/FontProvider.h
${GMPI_UI_SDK}/helpers/DecodeImage.h
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

if(UNIX AND NOT APPLE)
    # Mirrors wrapper/VST3's Linux block, dependency for dependency. X11 +
    # MIT-SHM for the embedded window, then the pieces the CPU renderer has no
    # code of its own for: fontconfig to find faces, freetype to rasterise,
    # harfbuzz to shape, libpng to decode. The helper headers select their
    # implementation with __has_include, so a MISSING include path here does
    # not fail the build -- it silently compiles a plugin that draws no text.
    # That is why these are REQUIRED and PUBLIC rather than best-effort.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GMPI_CLAP_X11        REQUIRED x11 xext)
    pkg_check_modules(GMPI_CLAP_FONTCONFIG REQUIRED fontconfig)
    pkg_check_modules(GMPI_CLAP_FREETYPE   REQUIRED freetype2)
    pkg_check_modules(GMPI_CLAP_HARFBUZZ   REQUIRED harfbuzz)
    pkg_check_modules(GMPI_CLAP_LIBPNG     REQUIRED libpng)
    # The X11 frame's file chooser goes through the XDG desktop portal, which
    # is D-Bus -- not a Wayland-only dependency.
    pkg_check_modules(GMPI_CLAP_DBUS       REQUIRED dbus-1)

    target_include_directories(${PROJECT_NAME} PUBLIC
        ${GMPI_CLAP_X11_INCLUDE_DIRS}
        ${GMPI_CLAP_FONTCONFIG_INCLUDE_DIRS}
        ${GMPI_CLAP_FREETYPE_INCLUDE_DIRS}
        ${GMPI_CLAP_HARFBUZZ_INCLUDE_DIRS}
        ${GMPI_CLAP_LIBPNG_INCLUDE_DIRS}
        ${GMPI_CLAP_DBUS_INCLUDE_DIRS}
    )

    # Plain signature deliberately: every target_link_libraries on this target
    # uses it, and CMake refuses to mix plain and keyword forms on one target.
    target_link_libraries(${PROJECT_NAME}
        ${GMPI_CLAP_X11_LIBRARIES}
        ${GMPI_CLAP_FONTCONFIG_LIBRARIES}
        ${GMPI_CLAP_FREETYPE_LIBRARIES}
        ${GMPI_CLAP_HARFBUZZ_LIBRARIES}
        ${GMPI_CLAP_LIBPNG_LIBRARIES}
        ${GMPI_CLAP_DBUS_LIBRARIES}
    )
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
