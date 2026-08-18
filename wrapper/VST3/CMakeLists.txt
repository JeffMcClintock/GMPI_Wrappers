cmake_minimum_required(VERSION 3.30)

project(VST3_Wrapper)

set(CMAKE_CXX_STANDARD 20)

add_definitions(-D_UNICODE)

if (MSVC)
    # Floating Point Model: Fast (/fp:fast)
    # Buffer Security Check: No (/GS-)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /fp:fast /GS-")
endif()

set(srcs
# Host-side scaffolding, shared with the CLAP/AU/standalone wrappers - one list,
# in wrapper/CMakeLists.txt.
${GMPI_HOSTING_SRCS}

MyVstPluginFactory.h
MyVstPluginFactory.cpp
Processor_VST3.h
Processor_VST3.cpp
Controller_VST3.h
Controller_VST3.cpp
VST3EditorBase.h
VST3EditorBase.cpp

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
SEVSTGUIEditorWin.h
SEVSTGUIEditorWin.cpp
${VST3_SDK}/public.sdk/source/main/dllmain.cpp
)
ENDIF()

if(APPLE)
set(srcs ${srcs}
SEVSTGUIEditorMac.h
SEVSTGUIEditorMac.cpp
${GMPI_UI_SDK}/backends/DrawingFrameMac.mm
${GMPI_UI_SDK}/backends/CocoaGfx.h
${GMPI_UI_SDK}/backends/Gfx_base.h
${GMPI_UI_SDK}/backends/Bezier.h
${VST3_SDK}/public.sdk/source/main/macmain.cpp
)
ENDIF()

if(UNIX AND NOT APPLE)
# Linux: no linuxmain.cpp here. It defines ModuleEntry/ModuleExit, which nothing
# in this static library references, so the linker would never pull the object
# in. Those entry points are defined in wrapperVst3.cpp instead, which
# gmpi_plugin.cmake compiles straight into each plugin module.
#
# The X11 editor. Until VST3 3.8.0 this was the only Linux embedding the
# specification defined, and it is still what a host reaches us through when it
# offers no Wayland (including XWayland sessions). The CPU renderer needs the
# same text/image stack the standalone Wayland editor links.
set(srcs ${srcs}
SEVSTGUIEditorLinux.h
SEVSTGUIEditorLinux.cpp
${GMPI_UI_SDK}/backends/DrawingFrameX11.h
${GMPI_UI_SDK}/backends/DrawingFrameX11.cpp
${GMPI_UI_SDK}/backends/CpuGfx.h
${GMPI_UI_SDK}/backends/CpuEncode.h
${GMPI_UI_SDK}/helpers/CpuTextEngine.h
${GMPI_UI_SDK}/helpers/FontProvider.h
${GMPI_UI_SDK}/helpers/DecodeImage.h
)

# Wayland (VST3 3.8.0's kPlatformTypeWaylandSurfaceID). Optional: an SDK that
# predates 3.8.0, or a build machine without the Wayland development packages,
# gets the X11 editor and nothing else changes.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GMPI_WL      wayland-client)
pkg_check_modules(GMPI_XKB     xkbcommon)
pkg_check_modules(GMPI_DECOR   libdecor-0)

# The SDK half of that promise. iwaylandframe.h arrived in 3.8.0 and is the one
# header SEVSTGUIEditorWayland cannot do without, so its presence is the test -
# cheaper to trust than a version string, since the SDK may be a local override
# at any commit. Without it the Wayland packages alone would switch the editor
# on and the include would fail.
set(GMPI_VST3_SDK_HAS_WAYLAND FALSE)
if(EXISTS "${VST3_SDK}/pluginterfaces/gui/iwaylandframe.h")
    set(GMPI_VST3_SDK_HAS_WAYLAND TRUE)
endif()

set(GMPI_VST3_WAYLAND FALSE)
if(GMPI_WL_FOUND AND GMPI_XKB_FOUND AND GMPI_DECOR_FOUND AND GMPI_VST3_SDK_HAS_WAYLAND)
    # Protocol bindings are generated, not shipped: wayland-scanner emits a
    # header plus a C file of message signatures per protocol. The set, the
    # search for the XML files and the generation are shared with the standalone
    # app's shell, which embeds the same gmpi_ui Wayland backend and therefore
    # needs bindings for exactly the same protocols.
    #
    # This call resolves everything before it generates anything, which is the
    # whole point of sharing it. The copy of the loop that used to live here
    # treated the libraries above as proof the XML files existed: on a
    # distribution older than wayland-protocols 1.32 - Ubuntu 22.04 ships 1.25 -
    # the gate switched Wayland on and the build then died at a custom command
    # whose DEPENDS named a file that was not there, with no way to point it
    # anywhere else.
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GmpiWayland.cmake")
    gmpi_wayland_protocols(WL_PROTOCOL_SOURCES WL_GEN WL_PROTOCOLS_MISSING)

    if(WL_PROTOCOLS_MISSING)
        # Named individually, because "install wayland-protocols" is the wrong
        # advice when the package is present but predates the staging protocols
        # the backend binds. Falling back to X11 rather than failing: this
        # wrapper has a second Linux editor and it works.
        list(JOIN WL_PROTOCOLS_MISSING ", " _wl_missing_pretty)
        message(STATUS "VST3 wrapper: Wayland support off -- missing "
                       "${_wl_missing_pretty}. Point GMPI_WAYLAND_PROTOCOLS_DIR "
                       "at a newer wayland-protocols tree to build it anyway; "
                       "X11 editor only.")
    else()
        set(GMPI_VST3_WAYLAND TRUE)

        set(srcs ${srcs}
            SEVSTGUIEditorWayland.h
            SEVSTGUIEditorWayland.cpp
            ${GMPI_UI_SDK}/backends/DrawingFrameWayland.h
            ${WL_PROTOCOL_SOURCES}
        )
    endif()
else()
    message(STATUS "VST3 wrapper: Wayland support off (need wayland-client, "
                   "xkbcommon, libdecor-0 and a VST3 SDK of 3.8.0 or later); "
                   "X11 editor only.")
endif()

# The tests directory is a sibling added by wrapper/CMakeLists.txt, so it only
# sees what we push up. Pushed in both cases: OFF has to overwrite whatever is
# in that scope, not merely decline to set anything.
set(GMPI_VST3_WAYLAND ${GMPI_VST3_WAYLAND} PARENT_SCOPE)
endif()

set(vst_gui_minimal_srcs
    ${VST3_SDK}/pluginterfaces/base/ustring.h
    ${VST3_SDK}/pluginterfaces/base/ustring.cpp
    ${VST3_SDK}/pluginterfaces/base/funknown.h
    ${VST3_SDK}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK}/pluginterfaces/base/coreiids.cpp
    ${VST3_SDK}/pluginterfaces/base/ipluginbase.h 
    ${VST3_SDK}/base/source/baseiids.cpp
    ${VST3_SDK}/base/source/fdebug.h
    ${VST3_SDK}/base/source/fdebug.cpp
    ${VST3_SDK}/base/source/fobject.h
    ${VST3_SDK}/base/source/fobject.cpp
    ${VST3_SDK}/base/source/fstring.h
    ${VST3_SDK}/base/source/fstring.cpp
    ${VST3_SDK}/base/source/updatehandler.h
    ${VST3_SDK}/base/source/updatehandler.cpp
    ${VST3_SDK}/base/thread/source/flock.cpp
    
    ${VST3_SDK}/public.sdk/source/common/pluginview.h
    ${VST3_SDK}/public.sdk/source/common/pluginview.cpp
    ${VST3_SDK}/public.sdk/source/common/commoniids.cpp

    ${VST3_SDK}/public.sdk/source/vst/vstbus.h
    ${VST3_SDK}/public.sdk/source/vst/vstbus.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstcomponentbase.h
    ${VST3_SDK}/public.sdk/source/vst/vstcomponentbase.cpp

    ${VST3_SDK}/public.sdk/source/vst/vsteditcontroller.h
    ${VST3_SDK}/public.sdk/source/vst/vsteditcontroller.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp

    ${VST3_SDK}/public.sdk/source/vst/vstcomponent.h
    ${VST3_SDK}/public.sdk/source/vst/vstcomponent.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstaudioeffect.h
    ${VST3_SDK}/public.sdk/source/vst/vstaudioeffect.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstparameters.h
    ${VST3_SDK}/public.sdk/source/vst/vstparameters.cpp 
)

if(VST3_SDK_USE_MIDI_UMP)
set(vst_gui_minimal_srcs ${vst_gui_minimal_srcs}
    ${VST3_MIDI_UMP_SDK}/ivstmidiump.h
    ${VST3_MIDI_UMP_SDK}/ivstumpevents.h
    ${VST3_MIDI_UMP_SDK}/vstmidiumpiids.cpp
)
endif()

include_directories(
${VST3_SDK}
${GMPI_SDK}
${GMPI_SDK}/Core
${GMPI_UI_SDK}
${GMPI_UI_SDK}/helpers
${ADAPTORS_COMMON_FOLDER}
)

source_group(se_sdk FILES ${sdk_srcs})

if(APPLE)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GmpiFrameworks.cmake")
gmpi_find_frameworks(vst3_frameworks
    CoreFoundation
    QuartzCore
    Cocoa
    CoreText
    OpenGL
    ImageIO
    CoreGraphics
    Accelerate
)
endif()

include (GenerateExportHeader)

add_library(${PROJECT_NAME} STATIC ${sdk_srcs} ${srcs} ${vst_gui_minimal_srcs})

# 'DEBUG' is a steinberg thing, ('_DEBUG' is standard C++)
target_compile_definitions(${PROJECT_NAME} PRIVATE 
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Debug>:DEBUG=1>
  $<$<CONFIG:Release>:DEBUG=0>
  $<$<CONFIG:Release>:NDEBUG>
)

if(APPLE)
# Plain signature, like every other target_link_libraries on this target - CMake
# refuses to mix the plain and keyword forms on one.
target_link_libraries(${PROJECT_NAME} ${vst3_frameworks})
endif()

if(CMAKE_HOST_WIN32)
target_link_libraries(${PROJECT_NAME} Dwmapi dxguid d3d11 d2d1 dwrite windowscodecs)
endif()

if(UNIX AND NOT APPLE)
    # X11 + MIT-SHM for the embedded window, then the pieces the CPU renderer
    # has no code of its own for: fontconfig to find faces, freetype to
    # rasterise, harfbuzz to shape, libpng for image decode. The helper headers
    # select their implementation with __has_include, so a missing include path
    # here does not fail the build - it silently compiles a plugin that draws no
    # text. PUBLIC so each plugin module inherits them.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GMPI_X11 REQUIRED x11 xext)
    pkg_check_modules(GMPI_FONTCONFIG REQUIRED fontconfig)
    pkg_check_modules(GMPI_FREETYPE REQUIRED freetype2)
    pkg_check_modules(GMPI_HARFBUZZ REQUIRED harfbuzz)
    pkg_check_modules(GMPI_LIBPNG REQUIRED libpng)
    # Not Wayland-only: the X11 frame's file chooser goes through the same XDG
    # desktop portal, which is D-Bus.
    pkg_check_modules(GMPI_DBUS REQUIRED dbus-1)

    target_include_directories(${PROJECT_NAME} PUBLIC
        ${GMPI_X11_INCLUDE_DIRS}
        ${GMPI_FONTCONFIG_INCLUDE_DIRS}
        ${GMPI_FREETYPE_INCLUDE_DIRS}
        ${GMPI_HARFBUZZ_INCLUDE_DIRS}
        ${GMPI_LIBPNG_INCLUDE_DIRS}
        ${GMPI_DBUS_INCLUDE_DIRS}
    )

    if(GMPI_VST3_WAYLAND)
        target_include_directories(${PROJECT_NAME} PUBLIC
            ${WL_GEN}                       # generated *-client-protocol.h
            ${GMPI_WL_INCLUDE_DIRS}
            ${GMPI_XKB_INCLUDE_DIRS}
            ${GMPI_DECOR_INCLUDE_DIRS}
        )
        target_link_libraries(${PROJECT_NAME}
            ${GMPI_WL_LIBRARIES}
            ${GMPI_XKB_LIBRARIES}
            ${GMPI_DECOR_LIBRARIES}
        )
        target_compile_definitions(${PROJECT_NAME} PUBLIC GMPI_VST3_WAYLAND=1)
    endif()
    # Plain signature deliberately: every target_link_libraries on this target
    # uses it, the Windows and macOS ones included, and CMake refuses to mix
    # plain and keyword forms on one target.
    target_link_libraries(${PROJECT_NAME}
        ${GMPI_X11_LIBRARIES}
        ${GMPI_FONTCONFIG_LIBRARIES}
        ${GMPI_FREETYPE_LIBRARIES}
        ${GMPI_HARFBUZZ_LIBRARIES}
        ${GMPI_LIBPNG_LIBRARIES}
        ${GMPI_DBUS_LIBRARIES}
    )
endif()

target_compile_definitions (${PROJECT_NAME} PRIVATE -D_UNICODE -DUNICODE)

if(VST3_SDK_USE_MIDI_UMP)
target_compile_definitions (${PROJECT_NAME} PRIVATE VST3_USE_MIDI_EXTENSION)
endif()

if(APPLE)
set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE TRUE)
set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE_EXTENSION vst3)
endif()

# required by Linux build
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

########################### VST3 SDK #######################################

list(APPEND CMAKE_MODULE_PATH "${VST3_SDK}/cmake/modules")

include(SMTG_VST3_SDK)

#---Add base libraries---------------------------
set(VST_SDK TRUE) # used for Base module which provides only a subset of Base for VST-SDK
SET_TARGET_PROPERTIES(${PROJECT_NAME} PROPERTIES FOLDER "wrappers")
