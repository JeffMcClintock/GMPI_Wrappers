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
