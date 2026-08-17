cmake_minimum_required(VERSION 3.30)

project(Standalone_Wrapper)

# The standalone "wrapper": everything a bare application needs in order to
# host one statically-linked GMPI plugin. A peer to VST3_Wrapper / CLAP_Wrapper
# / AU_Wrapper, and built the same way - a static library the plugin's own
# target links, with the entry point added to the EXECUTABLE rather than to
# this library (a main() inside a static archive is never pulled in, because
# nothing references it).
#
# Linux only for now. The portable half compiles anywhere, but without a
# platform shell there is no window and no main, so a Standalone target on
# Windows or macOS would configure and then fail to link. Better to say so.

set(CMAKE_CXX_STANDARD 20)

if(NOT UNIX OR APPLE)
    message(STATUS "Standalone_Wrapper: skipped (only the Linux/Wayland shell is written)")
    return()
endif()

# This target's dependencies are DISCOVERED, not REQUIRED, and a missing one
# skips the target rather than failing the configure.
#
# The parent's add_subdirectory() promises "safe to add unconditionally", and
# with REQUIRED probes that promise was false: on a Linux box without the
# pipewire SDK the whole tree stopped configuring, so nothing at all could be
# built -- no VST3, no CLAP, no plugin of any kind -- over an optional bare-app
# target that, today, nothing links. Windows and macOS never saw it because the
# early-out above returns before these lines.
#
# A hard dependency is the right call for something a product ships; this is a
# developer convenience with no consumers yet, so the cost of stopping every
# other target is far larger than the cost of quietly not building it. See
# TideSynth issue #53.
#
# Set GMPI_STANDALONE_STRICT=ON to turn the skip back into a hard error --
# for CI, or for a developer who means to build this and wants to be told why
# it is not happening rather than reading configure output for it.
option(GMPI_STANDALONE_STRICT
    "Fail the configure if Standalone_Wrapper's dependencies are missing, rather than skipping it" OFF)

find_package(PkgConfig REQUIRED)

set(_standalone_missing "")

# name <-> pkg-config module. Probed without REQUIRED so a miss is recoverable.
foreach(dep
    "WL:wayland-client"
    "XKB:xkbcommon"
    "DECOR:libdecor-0"
    "DBUS:dbus-1"
    "FONTCONFIG:fontconfig"
    "HARFBUZZ:harfbuzz"
    "FREETYPE:freetype2"
    "ALSA:alsa"
    "PIPEWIRE:libpipewire-0.3"
    # Was linked as a bare `png` and reached through /usr/include/png.h, which
    # is a Debian-ism: the canonical header lives in libpng16/, and pkg-config
    # is what knows the current soname. Probed properly now that the command
    # channel's screenshot verb includes <png.h> directly.
    "PNG:libpng"
)
    string(REPLACE ":" ";" _pair ${dep})
    list(GET _pair 0 _prefix)
    list(GET _pair 1 _module)
    # Deliberately not QUIET: the per-module "Found X, version Y" lines are how
    # you tell "absent" from "present but too old", and the summary below only
    # names what was missing.
    pkg_check_modules(${_prefix} ${_module})
    if(NOT ${_prefix}_FOUND)
        list(APPEND _standalone_missing ${_module})
    endif()
endforeach()

# Wayland protocol bindings are generated, not shipped: wayland-scanner emits a
# header plus a C file of message signatures for each protocol the backend
# binds. The set is dictated by backends/DrawingFrameWayland.h.
pkg_get_variable(WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner)
if(NOT WAYLAND_SCANNER)
    list(APPEND _standalone_missing "wayland-scanner")
endif()

# fractional-scale-v1 and cursor-shape-v1 are staging protocols that landed in
# wayland-protocols 1.31 and 1.32; a distro older than that (Ubuntu 22.04 ships
# 1.25) has the scanner and the client library but not these XML files. Point
# this at a directory laid out like wayland-protocols' own and it is searched
# first, so an old build host can be topped up without replacing its packages.
set(GMPI_WAYLAND_PROTOCOLS_DIR "" CACHE PATH
    "Extra wayland-protocols tree searched before the system one")

set(WL_PROTOCOL_PATHS
    stable/xdg-shell/xdg-shell
    stable/viewporter/viewporter
    staging/fractional-scale/fractional-scale-v1
    staging/cursor-shape/cursor-shape-v1
    unstable/tablet/tablet-unstable-v2          # cursor-shape-v1 references it
    unstable/xdg-foreign/xdg-foreign-unstable-v2
)

# Resolve every protocol XML BEFORE generating anything, so a missing one joins
# the same missing-dependency list as the pkg-config modules and is reported in
# one message. Resolving and generating in a single loop would emit custom
# commands for the protocols found so far and only then discover the gap.
set(_wl_protocol_xml "")
foreach(proto ${WL_PROTOCOL_PATHS})
    set(xml "")
    foreach(dir ${GMPI_WAYLAND_PROTOCOLS_DIR} ${WL_PROTOCOLS_DIR})
        if(EXISTS "${dir}/${proto}.xml")
            set(xml "${dir}/${proto}.xml")
            break()
        endif()
    endforeach()

    # Named individually rather than as "wayland-protocols", because the usual
    # cause is a distro too old for the staging protocols rather than the
    # package being absent -- Ubuntu 22.04 ships 1.25 and has neither
    # fractional-scale-v1 nor cursor-shape-v1. Naming the file is what tells
    # those two cases apart, and GMPI_WAYLAND_PROTOCOLS_DIR is the fix for the
    # first without touching system packages.
    if(NOT xml)
        list(APPEND _standalone_missing "${proto}.xml (wayland-protocols >= 1.32)")
    else()
        list(APPEND _wl_protocol_xml ${xml})
    endif()
endforeach()

# The single gate. Everything above only probed; nothing below runs unless the
# whole set is present.
if(_standalone_missing)
    list(JOIN _standalone_missing "\n    " _standalone_missing_pretty)
    if(GMPI_STANDALONE_STRICT)
        message(FATAL_ERROR
            "Standalone_Wrapper: GMPI_STANDALONE_STRICT is ON and these dependencies are missing:\n"
            "    ${_standalone_missing_pretty}\n"
            "On Debian/Ubuntu: sudo apt install libwayland-dev libxkbcommon-dev libdecor-0-dev "
            "libdbus-1-dev libfontconfig-1-dev libharfbuzz-dev libfreetype-dev libasound2-dev "
            "libpipewire-0.3-dev wayland-protocols")
    endif()
    message(STATUS
        "Standalone_Wrapper: skipped -- missing:\n"
        "    ${_standalone_missing_pretty}\n"
        "  Every other target still builds. To build the standalone host: "
        "sudo apt install libpipewire-0.3-dev libasound2-dev wayland-protocols "
        "(and see the list above). Set GMPI_STANDALONE_STRICT=ON to make this fatal.")
    return()
endif()

set(WL_GEN ${CMAKE_CURRENT_BINARY_DIR}/wl-gen)
file(MAKE_DIRECTORY ${WL_GEN})

set(WL_PROTOCOL_SOURCES "")
foreach(xml ${_wl_protocol_xml})
    # NAME then strip .xml, rather than NAME_WE: NAME_WE removes the *longest*
    # extension, so a protocol whose filename contained another dot would come
    # out truncated. None does today; this does not depend on that staying true.
    get_filename_component(name ${xml} NAME)
    string(REGEX REPLACE "\\.xml$" "" name ${name})

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

set(srcs
    # Host-side scaffolding, shared with the VST3/CLAP/AU wrappers.
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

    ${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.h
    ${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.cpp

    # Portable half of the app.
    AudioMidiDevices.h
    StandaloneHost.h
    StandaloneHost.cpp
    StandaloneSettings.h
    StandaloneSettings.cpp
    AppLayout.h
    AppLayout.cpp
    MenuBarView.h
    MenuBarView.cpp
    SettingsPane.h
    SettingsPane.cpp

    # Linux shell (the entry point itself is added to the executable).
    linux/AudioDriverPipeWire.h
    linux/AudioDriverPipeWire.cpp
    linux/MidiDriverAlsa.h
    linux/MidiDriverAlsa.cpp

    # The command channel: a unix socket that drives this app's plugin, so a
    # test or an MCP server can exercise the running instance. Everything
    # except the socket itself is portable, and only IpcServer.h is POSIX.
    mcp/MainThreadQueue.h
    mcp/IpcServer.h
    mcp/TextUtil.h
    mcp/PngWriter.h
    mcp/WavWriter.h
    mcp/CommandDispatcher.h
    mcp/CommandDispatcher.cpp

    # gmpi_ui pieces the shell draws with. Header-only apart from these.
    ${GMPI_UI_SDK}/helpers/Timer.h
    ${GMPI_UI_SDK}/helpers/Timer.cpp
    ${GMPI_UI_SDK}/experimental/builders.cpp
    ${GMPI_UI_SDK}/experimental/forms.cpp
    ${GMPI_UI_SDK}/experimental/grid.cpp
    ${GMPI_UI_SDK}/experimental/primatives.cpp
    ${GMPI_UI_SDK}/experimental/ui_facades.cpp

    ${WL_PROTOCOL_SOURCES}
)

add_library(${PROJECT_NAME} STATIC ${srcs})

# PUBLIC so the executable that links this gets the generated protocol headers
# and the SDK include paths without repeating them - it compiles MainWayland.cpp
# itself, which includes backends/DrawingFrameWayland.h.
target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WL_GEN}
    ${GMPI_SDK}
    ${GMPI_SDK}/Core
    ${GMPI_UI_SDK}
    ${ADAPTORS_COMMON_FOLDER}
    ${WL_INCLUDE_DIRS}
    ${XKB_INCLUDE_DIRS}
    ${DECOR_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${HARFBUZZ_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    ${PIPEWIRE_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
)

# PRIVATE, not PUBLIC: compat/ holds a shim for a SynthEdit SDK header that
# gmpi_ui's widget set includes (see compat/it_enum_list.h). Only the
# translation units compiled here need it, and leaking it to consumers could
# shadow the real thing in a project that has both.
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:RelWithDebInfo>:NDEBUG>
)

target_link_libraries(${PROJECT_NAME} PUBLIC
    ${WL_LIBRARIES}
    ${XKB_LIBRARIES}
    ${DECOR_LIBRARIES}
    ${DBUS_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    ${HARFBUZZ_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${ALSA_LIBRARIES}
    ${PIPEWIRE_LIBRARIES}
    ${PNG_LIBRARIES}
)

# Directories the pkg-config modules were found in. Needed when the libraries
# live outside the linker's default path, which is the case for any user-local
# prefix (see the WSL note in the repo's build docs).
target_link_directories(${PROJECT_NAME} PUBLIC
    ${WL_LIBRARY_DIRS}
    ${XKB_LIBRARY_DIRS}
    ${DECOR_LIBRARY_DIRS}
    ${DBUS_LIBRARY_DIRS}
    ${FONTCONFIG_LIBRARY_DIRS}
    ${HARFBUZZ_LIBRARY_DIRS}
    ${FREETYPE_LIBRARY_DIRS}
    ${ALSA_LIBRARY_DIRS}
    ${PIPEWIRE_LIBRARY_DIRS}
    ${PNG_LIBRARY_DIRS}
)

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "wrappers")
