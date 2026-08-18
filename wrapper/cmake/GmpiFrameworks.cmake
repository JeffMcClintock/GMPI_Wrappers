# Finding macOS system frameworks, for the four wrappers that link them.
#
# Here rather than in the GMPI SDK for the same reason as GmpiWayland.cmake:
# every caller is in this repo, so the include can be plain rather than the
# OPTIONAL-and-cope-with-absence that a cross-repo include needs.
#
# The SDK is however a READER of what this produces - see the note on the cache
# variable names below - so the names are a contract, not a detail.

include_guard(GLOBAL)

# gmpi_find_frameworks(<out_var> <framework names...>)
#
# Finds each framework, hides its cache entry from the ccmake/gui default view,
# and returns the whole set in out_var for a single target_link_libraries call.
#
# A framework that is not found is returned as CMake's <VAR>-NOTFOUND, exactly
# as the hand-rolled FIND_LIBRARY blocks this replaces did: it surfaces as a
# link error naming the framework. Deliberately not upgraded to a configure-time
# error - nobody working on this repo can build macOS, so the safe change is the
# one that cannot turn a working build into a failing one.
function(gmpi_find_frameworks out_var)
    set(libs "")

    foreach(name ${ARGN})
        string(TOUPPER "${name}" upper)

        # <NAME>_LIBRARY, and that spelling is load-bearing rather than a
        # convention: gmpi_target() in the GMPI SDK's gmpi_plugin.cmake links
        # ${COREFOUNDATION_LIBRARY}, ${COCOA_LIBRARY} and six more by exactly
        # these names, and the only thing that ever fills those cache entries in
        # is a wrapper having run find_library first. Rename them here and every
        # plugin module silently links no frameworks - which is a macOS-only
        # failure, on the one platform this repo cannot build.
        find_library(${upper}_LIBRARY ${name})
        mark_as_advanced(${upper}_LIBRARY)

        list(APPEND libs ${${upper}_LIBRARY})
    endforeach()

    set(${out_var} "${libs}" PARENT_SCOPE)
endfunction()
