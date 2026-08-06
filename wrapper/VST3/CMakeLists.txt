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
${GMPI_SDK}/Hosting/parse_enum.h
${GMPI_SDK}/Hosting/xml_spec_reader.h
${GMPI_SDK}/Hosting/xml_spec_reader.cpp
${GMPI_SDK}/Hosting/controller_holder.h
${GMPI_SDK}/Hosting/controller_holder.cpp
${GMPI_SDK}/Hosting/processor_holder.h
${GMPI_SDK}/Hosting/processor_holder.cpp
${GMPI_SDK}/Hosting/message_queues.h
${GMPI_SDK}/Hosting/message_queues.cpp
${GMPI_SDK}/Hosting/gmpi_factory.h
${GMPI_SDK}/Hosting/gmpi_factory.cpp

MyVstPluginFactory.h
MyVstPluginFactory.cpp
Processor_VST3.h
Processor_VST3.cpp
Controller_VST3.h
Controller_VST3.cpp
VST3EditorBase.h
VST3EditorBase.cpp
${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.h
${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.cpp

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
find_program(WAYLAND_SCANNER wayland-scanner)

# The SDK half of that promise. iwaylandframe.h arrived in 3.8.0 and is the one
# header SEVSTGUIEditorWayland cannot do without, so its presence is the test -
# cheaper to trust than a version string, since the SDK may be a local override
# at any commit. Without it the Wayland packages alone would switch the editor
# on and the include would fail.
set(GMPI_VST3_SDK_HAS_WAYLAND FALSE)
if(EXISTS "${VST3_SDK}/pluginterfaces/gui/iwaylandframe.h")
    set(GMPI_VST3_SDK_HAS_WAYLAND TRUE)
endif()

if(GMPI_WL_FOUND AND GMPI_XKB_FOUND AND GMPI_DECOR_FOUND AND WAYLAND_SCANNER
   AND GMPI_VST3_SDK_HAS_WAYLAND)
    set(GMPI_VST3_WAYLAND TRUE)
    # The tests directory is a sibling added by wrapper/CMakeLists.txt, so it
    # only sees what we push up.
    set(GMPI_VST3_WAYLAND TRUE PARENT_SCOPE)

    # Protocol bindings are generated, not shipped: wayland-scanner emits a
    # header plus a C file of message signatures per protocol.
    pkg_get_variable(WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    set(WL_GEN ${CMAKE_CURRENT_BINARY_DIR}/wl-gen)
    file(MAKE_DIRECTORY ${WL_GEN})

    set(WL_PROTOCOL_PATHS
        stable/xdg-shell/xdg-shell
        stable/viewporter/viewporter
        staging/fractional-scale/fractional-scale-v1
        staging/cursor-shape/cursor-shape-v1
        unstable/tablet/tablet-unstable-v2          # cursor-shape-v1 references it
        unstable/xdg-foreign/xdg-foreign-unstable-v2
    )

    set(WL_PROTOCOL_SOURCES "")
    foreach(proto ${WL_PROTOCOL_PATHS})
        get_filename_component(name ${proto} NAME)
        set(xml ${WL_PROTOCOLS_DIR}/${proto}.xml)
        add_custom_command(
            OUTPUT ${WL_GEN}/${name}-client-protocol.h ${WL_GEN}/${name}-protocol.c
            COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${WL_GEN}/${name}-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} private-code  ${xml} ${WL_GEN}/${name}-protocol.c
            DEPENDS ${xml}
            COMMENT "wayland-scanner ${name}"
            VERBATIM
        )
        list(APPEND WL_PROTOCOL_SOURCES ${WL_GEN}/${name}-protocol.c)
    endforeach()

    set(srcs ${srcs}
        SEVSTGUIEditorWayland.h
        SEVSTGUIEditorWayland.cpp
        ${GMPI_UI_SDK}/backends/DrawingFrameWayland.h
        ${WL_PROTOCOL_SOURCES}
    )
else()
    message(STATUS "VST3 wrapper: Wayland support off (need wayland-client, "
                   "xkbcommon, libdecor-0, wayland-scanner and a VST3 SDK of "
                   "3.8.0 or later); X11 editor only.")
endif()
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
FIND_LIBRARY(COREFOUNDATION_LIBRARY CoreFoundation )
MARK_AS_ADVANCED (COREFOUNDATION_LIBRARY)
FIND_LIBRARY(QUARTZCORE_LIBRARY QuartzCore )
MARK_AS_ADVANCED (QUARTZCORE_LIBRARY)
FIND_LIBRARY(COCOA_LIBRARY Cocoa )
MARK_AS_ADVANCED (COCOA_LIBRARY)
FIND_LIBRARY(CORETEXT_LIBRARY CoreText )
MARK_AS_ADVANCED (CORETEXT_LIBRARY)
FIND_LIBRARY(OPENGL_LIBRARY OpenGL )
MARK_AS_ADVANCED (OPENGL_LIBRARY)
FIND_LIBRARY(IMAGEIO_LIBRARY ImageIO )
MARK_AS_ADVANCED (IMAGEIO_LIBRARY)
FIND_LIBRARY(COREGRAPHICS_LIBRARY CoreGraphics )
MARK_AS_ADVANCED (COREGRAPHICS_LIBRARY)
FIND_LIBRARY(ACCELERATE_LIBRARY Accelerate )
MARK_AS_ADVANCED (ACCELERATE_LIBRARY)
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

TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${COREFOUNDATION_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${COCOA_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${CORETEXT_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${OPENGL_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${IMAGEIO_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${COREGRAPHICS_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${ACCELERATE_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${QUARTZCORE_LIBRARY} )

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
    # Plain signature deliberately: the macOS framework links above use it, and
    # CMake refuses to mix plain and keyword forms on one target.
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
