# ---------------------------------------------------------------------------
# Host-side tests for the wrappers.
#
# Off by default: this directory is consumed by other projects (GMPI-plugins),
# and they should not grow a test executable unless they ask for one. Turn on
# with -DGMPI_WRAPPERS_BUILD_TESTS=ON.
# ---------------------------------------------------------------------------

project(gmpi_wrapper_tests)

# Explicit: the enclosing wrapper/ project sets 17, and the run loop uses
# std::erase_if.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Everything below this point was written for Linux and pkg-config's it
# unconditionally, so the whole directory used to be un-configurable anywhere
# else. Guard it by platform rather than by hand, so a Windows or macOS build
# with -DGMPI_WRAPPERS_BUILD_TESTS=ON gets the tests that apply to it instead of
# a pkg_check_modules failure.
if(UNIX AND NOT APPLE)

# x11_editor_host - loads a .vst3 bundle, opens its editor in a real X11 window,
# drives the Linux run loop, and reports whether anything was drawn. The
# Steinberg validator never calls attached(), so nothing else in the toolchain
# can tell a working editor from one that draws nothing.
add_executable(x11_editor_host
    x11_editor_host.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/module_linux.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK}/public.sdk/source/common/threadchecker_linux.cpp
    ${VST3_SDK}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3_SDK}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3_SDK}/public.sdk/source/common/commoniids.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp

    # The same minimal slice of base/pluginterfaces the plugin modules compile
    # in. There is no `base` or `pluginterfaces` LIBRARY target in this build -
    # the wrappers deliberately avoid add_subdirectory'ing the SDK - so linking
    # -lbase here would look for a system library that does not exist.
    ${VST3_SDK}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK}/pluginterfaces/base/coreiids.cpp
    ${VST3_SDK}/pluginterfaces/base/ustring.cpp
    ${VST3_SDK}/base/source/baseiids.cpp
    ${VST3_SDK}/base/source/fdebug.cpp
    ${VST3_SDK}/base/source/fobject.cpp
    ${VST3_SDK}/base/source/fstring.cpp
    ${VST3_SDK}/base/source/updatehandler.cpp
    ${VST3_SDK}/base/thread/source/flock.cpp
)

target_include_directories(x11_editor_host PRIVATE ${VST3_SDK})

find_package(PkgConfig REQUIRED)
pkg_check_modules(TEST_X11 REQUIRED x11)

target_include_directories(x11_editor_host PRIVATE ${TEST_X11_INCLUDE_DIRS})
target_link_libraries(x11_editor_host PRIVATE
    ${TEST_X11_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

set_target_properties(x11_editor_host PROPERTIES FOLDER "wrappers")

# ---------------------------------------------------------------------------
# wayland_editor_host - the same idea for the VST3 3.8.0 Wayland path. Needs the
# host to implement IWaylandHost / IWaylandFrame / Linux::IRunLoop, which is
# most of what this file is. Only built when the wrapper itself has Wayland.
# ---------------------------------------------------------------------------
if(GMPI_VST3_WAYLAND)
    pkg_check_modules(TEST_WL REQUIRED wayland-client)

    # Generate our own xdg-shell binding rather than reaching into the VST3
    # wrapper's build directory: a custom command's output belongs to the
    # directory that declared it.
    pkg_get_variable(TEST_WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    find_program(TEST_WAYLAND_SCANNER wayland-scanner REQUIRED)
    set(TEST_WL_GEN ${CMAKE_CURRENT_BINARY_DIR}/wl-gen)
    file(MAKE_DIRECTORY ${TEST_WL_GEN})

    add_custom_command(
        OUTPUT ${TEST_WL_GEN}/xdg-shell-client-protocol.h ${TEST_WL_GEN}/xdg-shell-protocol.c
        COMMAND ${TEST_WAYLAND_SCANNER} client-header
                ${TEST_WL_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
                ${TEST_WL_GEN}/xdg-shell-client-protocol.h
        COMMAND ${TEST_WAYLAND_SCANNER} private-code
                ${TEST_WL_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
                ${TEST_WL_GEN}/xdg-shell-protocol.c
        DEPENDS ${TEST_WL_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
        COMMENT "wayland-scanner xdg-shell (tests)"
        VERBATIM
    )

    add_executable(wayland_editor_host
        wayland_editor_host.cpp
        ${TEST_WL_GEN}/xdg-shell-protocol.c
        ${VST3_SDK}/public.sdk/source/vst/hosting/module.cpp
        ${VST3_SDK}/public.sdk/source/vst/hosting/module_linux.cpp
        ${VST3_SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
        ${VST3_SDK}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${VST3_SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
        ${VST3_SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
        ${VST3_SDK}/public.sdk/source/common/threadchecker_linux.cpp
        ${VST3_SDK}/public.sdk/source/vst/utility/stringconvert.cpp
        ${VST3_SDK}/public.sdk/source/common/commonstringconvert.cpp
        ${VST3_SDK}/public.sdk/source/common/commoniids.cpp
        ${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp

        ${VST3_SDK}/pluginterfaces/base/funknown.cpp
        ${VST3_SDK}/pluginterfaces/base/coreiids.cpp
        ${VST3_SDK}/pluginterfaces/base/ustring.cpp
        ${VST3_SDK}/base/source/baseiids.cpp
        ${VST3_SDK}/base/source/fdebug.cpp
        ${VST3_SDK}/base/source/fobject.cpp
        ${VST3_SDK}/base/source/fstring.cpp
        ${VST3_SDK}/base/source/updatehandler.cpp
        ${VST3_SDK}/base/thread/source/flock.cpp
    )

    target_include_directories(wayland_editor_host PRIVATE
        ${VST3_SDK}
        ${TEST_WL_GEN}
        ${TEST_WL_INCLUDE_DIRS}
    )
    target_link_libraries(wayland_editor_host PRIVATE
        ${TEST_WL_LIBRARIES}
        ${CMAKE_DL_LIBS}
    )
    set_target_properties(wayland_editor_host PROPERTIES FOLDER "wrappers")
endif()

endif() # UNIX AND NOT APPLE

# ---------------------------------------------------------------------------
# win_editor_resize_host - the regression test for the editor-resize crash.
# Attaches the editor in a real HWND and calls onSize with the oversized rect a
# host once passed, which used to dereference a released D2D device and kill the
# host process. See the file's own header for the full story.
#
# No SDK sources: ClassName_iid is header-only, so pluginterfaces headers plus
# user32 is the entire dependency. That is why this target does not carry the
# long source list the X11 one needs.
# ---------------------------------------------------------------------------
if(WIN32)
    add_executable(win_editor_resize_host win_editor_resize_host.cpp)
    target_include_directories(win_editor_resize_host PRIVATE ${VST3_SDK})
    target_link_libraries(win_editor_resize_host PRIVATE user32)
    set_target_properties(win_editor_resize_host PROPERTIES FOLDER "wrappers")
endif()
