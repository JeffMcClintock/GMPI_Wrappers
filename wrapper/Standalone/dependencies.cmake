# What Standalone_Wrapper needs, and whether this machine has it.
#
# Split out of CMakeLists.txt because two files have to ask the same question.
# This one asks it to decide whether to build the wrapper at all; the GMPI SDK's
# gmpi_plugin.cmake asks it to decide whether to create a plugin's STANDALONE
# executable, which links Standalone_Wrapper by name and so cannot be built if
# the wrapper declined.
#
# gmpi_plugin.cmake cannot simply test `if(TARGET Standalone_Wrapper)`: a plain
# name in target_link_libraries is not resolved until generate time, so a
# project may legally add_subdirectory() the wrappers AFTER the plugins that
# link them, and every consumer in this tree does. Nothing obliges a third-party
# one to order it the other way either, so no wrapper target can be relied on to
# exist at the point a plugin is defined. Probing is the only answer available
# that early, and this file exists so there is one probe rather than two that
# drift apart.
#
# Nothing here is ever fatal, a missing pkg-config included: a probe that stops
# the configure has not answered the question it was asked, and it stops it from
# inside gmpi_plugin(), where the casualties are every other target in the tree.
#
# Sets, in the includer's scope:
#
#   GMPI_STANDALONE_MISSING_DEPENDENCIES - empty when the wrapper can be built,
#       otherwise what is absent -- pkg-config itself, the pkg-config modules,
#       the tools and the protocol files -- in a form fit to print.
#
#   the pkg-config results (WL_*, XKB_*, ...), which CMakeLists.txt goes on to
#       build the target with -- but only when the list above came back empty.
#       A probe that gave up early leaves them unset, which is safe precisely
#       because that same list is what stops anything reading them.
#
# Only Linux has anything to probe: on Windows and macOS every dependency ships
# with the OS, so the list is empty by construction.

set(GMPI_STANDALONE_MISSING_DEPENDENCIES "")

if(UNIX AND NOT APPLE)

# Not REQUIRED: that would announce a missing pkg-config by killing the
# configure, the one outcome this mechanism exists to prevent. Nothing below can
# be looked for without it, so it is reported like any other absent dependency
# instead -- one more name in the list, and the wrapper declines as usual.
find_package(PkgConfig)

if(NOT PKG_CONFIG_FOUND)

list(APPEND GMPI_STANDALONE_MISSING_DEPENDENCIES "pkg-config")

else()

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
        list(APPEND GMPI_STANDALONE_MISSING_DEPENDENCIES ${_module})
    endif()
endforeach()

# Wayland protocol bindings are generated, not shipped: wayland-scanner emits a
# header plus a C file of message signatures for each protocol the backend
# binds. The set, the search for the XML files and the generator itself all live
# in the shared module, because the VST3 wrapper embeds the same Wayland backend
# and so needs the same bindings.
#
# RESOLVED here and never generated. This file is also included by the SDK's
# gmpi_plugin.cmake, once per plugin, purely to ask whether the wrapper can be
# built; a build rule emitted from there would land in the plugin's directory,
# and would become a duplicate OUTPUT -- a hard configure error -- the moment a
# project defines two plugins in one CMakeLists. CMakeLists.txt beside this file
# does the generating, once, in the wrapper's own directory.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GmpiWayland.cmake")

# The resolved paths are deliberately dropped: the generating call in
# CMakeLists.txt resolves again rather than being handed a list it would have to
# trust was complete, and the two cannot disagree - same function, same
# filesystem, no state in between.
gmpi_wayland_resolve_protocols(_wl_protocol_xml_ignored _wl_protocol_missing)
list(APPEND GMPI_STANDALONE_MISSING_DEPENDENCIES ${_wl_protocol_missing})

endif() # PKG_CONFIG_FOUND

endif() # UNIX AND NOT APPLE
