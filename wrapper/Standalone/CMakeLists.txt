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

find_package(PkgConfig REQUIRED)
pkg_check_modules(WL         REQUIRED wayland-client)
pkg_check_modules(XKB        REQUIRED xkbcommon)
pkg_check_modules(DECOR      REQUIRED libdecor-0)
pkg_check_modules(DBUS       REQUIRED dbus-1)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(HARFBUZZ   REQUIRED harfbuzz)
pkg_check_modules(FREETYPE   REQUIRED freetype2)
pkg_check_modules(ALSA       REQUIRED alsa)
pkg_check_modules(PIPEWIRE   REQUIRED libpipewire-0.3)

# Wayland protocol bindings are generated, not shipped: wayland-scanner emits a
# header plus a C file of message signatures for each protocol the backend
# binds. The set is dictated by backends/DrawingFrameWayland.h.
pkg_get_variable(WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# fractional-scale-v1 and cursor-shape-v1 are staging protocols that landed in
# wayland-protocols 1.31 and 1.32; a distro older than that (Ubuntu 22.04 ships
# 1.25) has the scanner and the client library but not these XML files. Point
# this at a directory laid out like wayland-protocols' own and it is searched
# first, so an old build host can be topped up without replacing its packages.
set(GMPI_WAYLAND_PROTOCOLS_DIR "" CACHE PATH
    "Extra wayland-protocols tree searched before the system one")

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

    set(xml "")
    foreach(dir ${GMPI_WAYLAND_PROTOCOLS_DIR} ${WL_PROTOCOLS_DIR})
        if(EXISTS "${dir}/${proto}.xml")
            set(xml "${dir}/${proto}.xml")
            break()
        endif()
    endforeach()

    # Fail HERE, naming the file. Left to the generator it surfaces as
    # "No rule to make target '<path>.xml'" at build time, which reads like a
    # broken CMakeLists rather than a missing package.
    if(NOT xml)
        message(FATAL_ERROR
            "Standalone_Wrapper: ${proto}.xml not found in ${WL_PROTOCOLS_DIR}. "
            "wayland-protocols 1.32 or later is required (or set "
            "GMPI_WAYLAND_PROTOCOLS_DIR to a tree that has it).")
    endif()

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
    png
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
)

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "wrappers")
