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

# main_thread_queue_deadline_probe - the command channel's two deadlines, and
# the heartbeat that lets a genuinely long job outlive the second one
# (BACKLOG E43). No window system, no plugin, no SDK: one header and the
# standard library, so it builds and runs identically on all three platforms
# and is deliberately OUTSIDE the guard below.
#
# NOT added to any test runner. It sleeps out two real 20-second deadlines, so
# it takes about a minute; that is affordable to run by hand after touching
# MainThreadQueue.h and not affordable per CI job. It also compiles in one
# command with no CMake at all - see the comment at the top of the file, which
# is the way it is expected to be run.
add_executable(main_thread_queue_deadline_probe main_thread_queue_deadline_probe.cpp)
find_package(Threads REQUIRED)
target_link_libraries(main_thread_queue_deadline_probe PRIVATE Threads::Threads)

# vst3_class_ids_test - the VST3 class IDs the VST3 factory reports: an explicit
# vst3ProcessorId/vst3ControllerId from the plugin descriptor, else the hashed
# fallback, pinned to the same GUID string on every platform. Runs in
# milliseconds; also compiles in one command (see the top of the file).
if(VST3_SDK AND GMPI_SDK)
    add_executable(vst3_class_ids_test
        vst3_class_ids_test.cpp
        ${GMPI_SDK}/Hosting/xml_spec_reader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../wrapper/common/tinyXml2/tinyxml2.cpp
        ${VST3_SDK}/pluginterfaces/base/funknown.cpp
    )
    target_include_directories(vst3_class_ids_test PRIVATE
        ${GMPI_SDK} ${GMPI_SDK}/Core
        ${CMAKE_CURRENT_SOURCE_DIR}/../wrapper/common
        ${VST3_SDK}
    )
    if(WIN32)
        target_compile_definitions(vst3_class_ids_test PRIVATE NOMINMAX)
        target_link_libraries(vst3_class_ids_test PRIVATE ole32)
    endif()
endif()

# vst3_state_read_test - the VST3 state reads: a chunk delivered in pieces, a
# short stream, and a corrupt size that must fail without being allocated.
# Header-only against the VST3 SDK; also compiles in one command (see the file).
if(VST3_SDK)
    add_executable(vst3_state_read_test vst3_state_read_test.cpp)
    target_include_directories(vst3_state_read_test PRIVATE ${VST3_SDK})
    if(WIN32)
        target_compile_definitions(vst3_state_read_test PRIVATE NOMINMAX)
    endif()
endif()

# controller_gesture_test - what the controller holder tells a wrapper when a GUI
# edits parameters: one host gesture per Grab, and private parameters routed to
# the processor and marked as a state change rather than performEdit'd with tag
# -1. Runs in milliseconds; also compiles in one command (see the top of the file).
if(GMPI_SDK AND GMPI_UI_SDK)
    add_executable(controller_gesture_test
        controller_gesture_test.cpp
        ${GMPI_SDK}/Hosting/controller_holder.cpp
        ${GMPI_SDK}/Hosting/xml_spec_reader.cpp
        ${GMPI_SDK}/Hosting/message_queues.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../wrapper/common/tinyXml2/tinyxml2.cpp
    )
    target_include_directories(controller_gesture_test PRIVATE
        ${GMPI_SDK} ${GMPI_SDK}/Core ${GMPI_UI_SDK}
        ${CMAKE_CURRENT_SOURCE_DIR}/../wrapper/common
    )
    if(WIN32)
        target_compile_definitions(controller_gesture_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

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

# ---------------------------------------------------------------------------
# mac_editor_resize_host - the same regression test on the Cocoa backend (P7).
#
# Same minimal-dependency trick as the Windows one: pluginterfaces headers plus
# system frameworks, no SDK hosting sources. The bundle is loaded through
# CFBundle rather than dlopen, so CoreFoundation is a real dependency and not
# just a convenience.
#
# Built as a plain executable, not a .app bundle: it needs a window server
# connection, which NSApplicationActivationPolicyRegular gets it, but it does
# not need an Info.plist or a bundle identifier to open an NSWindow.
# ---------------------------------------------------------------------------
if(APPLE)
    add_executable(mac_editor_resize_host mac_editor_resize_host.mm)
    target_include_directories(mac_editor_resize_host PRIVATE ${VST3_SDK})
    target_link_libraries(mac_editor_resize_host PRIVATE
        "-framework AppKit"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
    )
    set_target_properties(mac_editor_resize_host PROPERTIES FOLDER "wrappers")
endif()
