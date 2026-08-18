cmake_minimum_required(VERSION 3.30)

set(ADAPTORS_COMMON_FOLDER ${CMAKE_CURRENT_SOURCE_DIR}/common)

# The wrappers have always needed GMPI_SDK. What changed is that the list naming
# it lives here now rather than in each of them, so an empty variable reports
# itself as "cannot find source file /Hosting/parse_enum.h" in four
# subdirectories at once, none of which contains the line at fault. Say it once
# instead, where the line is.
if(NOT GMPI_SDK)
    message(FATAL_ERROR "The GMPI wrappers require GMPI_SDK to be set to the GMPI SDK folder.")
endif()

# The host-side scaffolding every wrapper compiles: the GMPI SDK's hosting layer
# and the XML parser xml_spec_reader.cpp is written against. One list, inherited
# by the four subdirectories below, so a file added to the hosting layer reaches
# VST3, CLAP, AU2 and the standalone app together instead of three of the four.
#
# A shared VARIABLE and not a static library, deliberately. These files are
# compiled today under four different sets of flags - AU2 builds at C++23 where
# the others use C++20, VST3 adds Steinberg's DEBUG=1/DEBUG=0, VST3 and CLAP add
# MSVC's /fp:fast /GS- and the other two do not - so one library target would
# have to pick a single set for everyone, which is a behaviour change wearing a
# refactor's clothes. It would also add a target to the link line of every
# plugin module, where each wrapper currently controls its own
# POSITION_INDEPENDENT_CODE and symbol visibility. Sharing the list changes
# nothing about how any of it is compiled.
#
# gmpi_plugin.cmake's plist_util keeps its own shorter list of three of these
# files: it runs in the CONSUMING project's scope, where nothing this directory
# sets is visible, and it is in the other repo besides.
set(GMPI_HOSTING_SRCS
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

    # Here rather than left to each wrapper: xml_spec_reader and both holders
    # include it, so a list that carried them without the parser would compile
    # and then fail to link.
    ${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.h
    ${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.cpp
)

add_subdirectory(VST3)
add_subdirectory(CLAP)

if(APPLE)
add_subdirectory(AU2)
endif()

# The bare app that wraps one plugin. Returns immediately on platforms whose
# shell is not written yet, so this is safe to add unconditionally.
add_subdirectory(Standalone)

# Off by default - consumers of this directory should not grow a test binary
# unless they ask for it.
option(GMPI_WRAPPERS_BUILD_TESTS "Build the wrappers' host-side tests" OFF)
# The platform gate moved into tests/CMakeLists.txt, which now picks the targets
# that apply: the X11/Wayland editor hosts on Linux, the editor-resize
# regression test on Windows. Still off by default either way.
if(GMPI_WRAPPERS_BUILD_TESTS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests gmpi_wrapper_tests)
endif()

