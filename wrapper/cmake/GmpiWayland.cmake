# Wayland protocol bindings, for every wrapper in this repo that draws on
# Wayland - the standalone app's shell and the VST3 wrapper's 3.8.0 editor.
#
# Both embed the SAME gmpi_ui Wayland backend, so both need bindings for the
# same set of protocols; a wrapper that generated a different set would be
# compiling that backend against a header it does not have. That is why one list
# can serve them, and why two lists could not stay true.
#
# Here rather than in the GMPI SDK next to gmpi_plugin.cmake, even though the
# SDK is where a plugin's formats are decided: every caller of these functions
# lives in THIS repo, and nothing in the SDK generates protocol bindings. The
# two repos are separate and can be checked out at mismatched revisions, which
# is why gmpi_plugin.cmake has to include Standalone/dependencies.cmake OPTIONAL
# and cope with its absence. A module included only by its own repo has no such
# problem: it is always at the same revision as the file including it, so the
# include can be plain and its absence correctly means a broken checkout.
#
# ---------------------------------------------------------------------------
#
# Two entry points, because the two phases have different callers:
#
#   gmpi_wayland_resolve_protocols(<out_xml_var> <out_missing_var>)
#       Finds every protocol XML, and the scanner that consumes them. Emits no
#       build rules at all, so it is safe to call from a scope that is only
#       asking a question. Standalone/dependencies.cmake needs exactly that:
#       gmpi_plugin.cmake includes it once per plugin to decide whether the
#       STANDALONE format can be built, and a custom command emitted from there
#       would become a duplicate OUTPUT - a hard configure error - the moment a
#       project defines two plugins in one CMakeLists.
#
#   gmpi_wayland_protocols(<out_sources_var> <out_include_dir_var> <out_missing_var>)
#       Resolves, and only then generates. Never interleaved, because a loop
#       that resolved and generated one protocol at a time would emit custom
#       commands for the protocols it had found so far and only afterwards
#       discover the gap. When anything is missing this generates nothing and
#       returns an empty source list; what to do about that is the caller's
#       policy - the standalone declines to build, the VST3 wrapper falls back
#       to its X11 editor.
#
# The second function is why this file exists. The VST3 wrapper used to carry
# its own copy of the loop with no EXISTS check, gated on the libraries and the
# scanner alone: on a distribution older than wayland-protocols 1.32 (Ubuntu
# 22.04 ships 1.25) that gate switched Wayland on and the build then died at the
# first custom command, whose DEPENDS named an XML that was not there.

include_guard(GLOBAL)

# Look for every protocol XML the gmpi_ui Wayland backend binds, plus
# wayland-scanner. Reports what is absent rather than failing, so the caller can
# decide - see the header above.
#
# The scanner is reported here rather than by each caller so that this
# function's promise holds: an empty missing list means generation cannot fail.
# An XML with no scanner to run over it is just as fatal as an absent XML.
function(gmpi_wayland_resolve_protocols out_xml_var out_missing_var)
    # The set is dictated by ${GMPI_UI_SDK}/backends/DrawingFrameWayland.h -
    # what that backend binds is what has to be generated.
    set(protocols
        stable/xdg-shell/xdg-shell
        stable/viewporter/viewporter
        staging/fractional-scale/fractional-scale-v1
        staging/cursor-shape/cursor-shape-v1
        unstable/tablet/tablet-unstable-v2          # cursor-shape-v1 references it
        unstable/xdg-foreign/xdg-foreign-unstable-v2
    )

    set(missing "")
    set(xml_files "")

    find_program(WAYLAND_SCANNER wayland-scanner)
    if(NOT WAYLAND_SCANNER)
        list(APPEND missing "wayland-scanner")
    endif()

    # QUIET, and never REQUIRED: a machine without pkg-config can still have
    # every protocol file, because GMPI_WAYLAND_PROTOCOLS_DIR below can supply
    # them all. Reporting the XMLs it actually lacks is more use than stopping
    # at the tool that would have located them, and the callers that care about
    # pkg-config for other reasons report its absence themselves.
    find_package(PkgConfig QUIET)
    set(system_dir "")
    if(PkgConfig_FOUND)
        pkg_get_variable(system_dir wayland-protocols pkgdatadir)
    endif()

    # fractional-scale-v1 and cursor-shape-v1 are staging protocols that landed
    # in wayland-protocols 1.31 and 1.32; a distro older than that has the
    # scanner and the client library but not these XML files. Point this at a
    # directory laid out like wayland-protocols' own and it is searched first,
    # so an old build host can be topped up without replacing its packages.
    set(GMPI_WAYLAND_PROTOCOLS_DIR "" CACHE PATH
        "Extra wayland-protocols tree searched before the system one")

    foreach(proto ${protocols})
        set(xml "")
        foreach(dir ${GMPI_WAYLAND_PROTOCOLS_DIR} ${system_dir})
            if(EXISTS "${dir}/${proto}.xml")
                set(xml "${dir}/${proto}.xml")
                break()
            endif()
        endforeach()

        if(xml)
            list(APPEND xml_files "${xml}")
        else()
            # Named individually rather than as "wayland-protocols", because the
            # usual cause is a distro too old for the staging protocols rather
            # than the package being absent. Naming the file is what tells those
            # two cases apart, and GMPI_WAYLAND_PROTOCOLS_DIR is the fix for the
            # first without touching system packages.
            list(APPEND missing "${proto}.xml (wayland-protocols >= 1.32)")
        endif()
    endforeach()

    set(${out_xml_var} "${xml_files}" PARENT_SCOPE)
    set(${out_missing_var} "${missing}" PARENT_SCOPE)
endfunction()

# Resolve, then run wayland-scanner over what was found. The generated headers
# and C files land in <caller's binary dir>/wl-gen, which is returned in
# out_include_dir_var - a function does not create a directory scope, so both
# CMAKE_CURRENT_BINARY_DIR and the custom commands below belong to the caller.
function(gmpi_wayland_protocols out_sources_var out_include_dir_var out_missing_var)
    gmpi_wayland_resolve_protocols(xml_files missing)

    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/wl-gen")
    set(sources "")

    # Nothing is generated when anything is missing: a custom command whose
    # DEPENDS does not exist is a build failure, and the caller asked to be told
    # instead.
    if(NOT missing)
        file(MAKE_DIRECTORY ${gen_dir})

        foreach(xml ${xml_files})
            # NAME then strip .xml, rather than NAME_WE: NAME_WE removes the
            # *longest* extension, so a protocol whose filename contained
            # another dot would come out truncated. None does today; this does
            # not depend on that staying true.
            get_filename_component(name ${xml} NAME)
            string(REGEX REPLACE "\\.xml$" "" name ${name})

            add_custom_command(
                OUTPUT ${gen_dir}/${name}-client-protocol.h ${gen_dir}/${name}-protocol.c
                COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${gen_dir}/${name}-client-protocol.h
                COMMAND ${WAYLAND_SCANNER} private-code  ${xml} ${gen_dir}/${name}-protocol.c
                DEPENDS ${xml}
                COMMENT "wayland-scanner ${name}"
                VERBATIM
            )
            list(APPEND sources ${gen_dir}/${name}-protocol.c)
        endforeach()
    endif()

    set(${out_sources_var} "${sources}" PARENT_SCOPE)
    set(${out_include_dir_var} "${gen_dir}" PARENT_SCOPE)
    set(${out_missing_var} "${missing}" PARENT_SCOPE)
endfunction()
