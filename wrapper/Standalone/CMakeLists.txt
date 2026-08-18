cmake_minimum_required(VERSION 3.30)

project(Standalone_Wrapper)

# The standalone "wrapper": everything a bare application needs in order to
# host one statically-linked GMPI plugin. A peer to VST3_Wrapper / CLAP_Wrapper
# / AU_Wrapper, and built the same way - a static library the plugin's own
# target links, with the entry point added to the EXECUTABLE rather than to
# this library (a main() inside a static archive is never pulled in, because
# nothing references it).
#
# Linux, Windows and macOS. The file is arranged as a portable source list plus
# one platform arm that appends the shell, its drivers and its dependencies; the
# add_library and the target_* calls at the bottom are shared.
#
# The three arms are not symmetrical, and the reason is worth stating: on Linux
# every dependency is a package that may be absent, so that arm probes for the
# whole set and can decline; on Windows and macOS they all ship with the OS, so
# there is nothing to probe and nothing that can decline. What the set is lives
# in dependencies.cmake, deliberately not restated here -- a second copy of a
# list is a second thing to keep true.

set(CMAKE_CXX_STANDARD 20)

if(NOT (WIN32 OR UNIX))
    message(STATUS "Standalone_Wrapper: skipped (no shell written for this platform)")
    return()
endif()

# ---------------------------------------------------------------------------
# The portable half. Compiles identically on every platform that gets this far.
# ---------------------------------------------------------------------------

set(standalone_srcs
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

    # gmpi_ui pieces the shell draws with. Header-only apart from these.
    ${GMPI_UI_SDK}/helpers/Timer.h
    ${GMPI_UI_SDK}/helpers/Timer.cpp
    ${GMPI_UI_SDK}/experimental/builders.cpp
    ${GMPI_UI_SDK}/experimental/forms.cpp
    ${GMPI_UI_SDK}/experimental/grid.cpp
    ${GMPI_UI_SDK}/experimental/primatives.cpp
    ${GMPI_UI_SDK}/experimental/ui_facades.cpp
)

set(standalone_include_dirs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GMPI_SDK}
    ${GMPI_SDK}/Core
    ${GMPI_UI_SDK}
    ${ADAPTORS_COMMON_FOLDER}
)

set(standalone_libs "")
set(standalone_library_dirs "")

# The command channel: an IPC endpoint that drives the plugin this app is
# hosting, so a test or an MCP server can exercise the running instance. ON
# because it is the only way a plugin's GUI is testable at all - see
# CommandChannel.h, which carries the same default so a translation unit
# compiled outside this build behaves identically.
#
# OFF removes the code rather than merely declining to start it: mcp/ is not
# added to the source list below, so a vendor who does not want a local IPC
# endpoint in a signed product has nothing left to audit.
option(GMPI_STANDALONE_COMMAND_CHANNEL
    "Build the standalone app's command channel (IPC for tests and the MCP server)" ON)

# The portable half of the channel. The transport is per-platform and is added
# by the arms below; everything here is the same code on both.
set(standalone_mcp_srcs
    mcp/MainThreadQueue.h
    mcp/IpcServer.h
    mcp/TextUtil.h
    mcp/PngWriter.h
    mcp/WavWriter.h
    mcp/CommandDispatcher.h
    mcp/CommandDispatcher.cpp
)

# ---------------------------------------------------------------------------
# Windows: Direct2D, WASAPI, winmm.
# ---------------------------------------------------------------------------

if(WIN32)
    list(APPEND standalone_srcs
        windows/ToplevelWindow.h
        windows/ToplevelWindow.cpp
        windows/AudioDriverWasapi.h
        windows/AudioDriverWasapi.cpp
        windows/MidiDriverWin.h
        windows/MidiDriverWin.cpp

        # The Direct2D editor frame, which is the same one the GMPI VST3
        # wrapper embeds in a DAW. windows/ToplevelWindow only supplies the
        # overlapped window it lives inside.
        ${GMPI_UI_SDK}/backends/DrawingFrameCommon.h
        ${GMPI_UI_SDK}/backends/DrawingFrameCommon.cpp
        ${GMPI_UI_SDK}/backends/DrawingFrameWin.h
        ${GMPI_UI_SDK}/backends/DrawingFrameWin.cpp
        ${GMPI_UI_SDK}/backends/DirectXGfx.h
        ${GMPI_UI_SDK}/backends/DirectXGfx.cpp
    )

    if(GMPI_STANDALONE_COMMAND_CHANNEL)
        list(APPEND standalone_srcs
            ${standalone_mcp_srcs}
            mcp/IpcServerWin.h            # the transport: a named pipe
            windows/FrameCapture.h        # --screenshot: swap-chain readback
            windows/FrameCapture.cpp
        )
    endif()

    list(APPEND standalone_libs
        d3d11 d2d1 dwrite windowscodecs   # the drawing backend, and WIC for PNG
        Dwmapi dxguid                     # window metrics, and the D3D/D2D IIDs
        ole32                             # COM, for WASAPI's device enumeration
        avrt                              # MMCSS "Pro Audio" thread scheduling
        winmm                             # MIDI input
    )
endif()

# ---------------------------------------------------------------------------
# macOS: CoreGraphics, CoreAudio, CoreMIDI.
#
# The thinnest of the three arms, because gmpi_ui's Cocoa backend is not a class
# this shell embeds but an NSVIEW it parents - the same view the AU, VST3 and
# CLAP wrappers hand their host. mac/ToplevelWindowMac supplies the NSWindow it
# lives in, and nothing else.
#
# Every dependency is a system framework, so unlike the Linux arm there is
# nothing to probe and nothing that can decline.
# ---------------------------------------------------------------------------

if(APPLE)
    list(APPEND standalone_srcs
        mac/ToplevelWindowMac.h
        mac/ToplevelWindowMac.mm
        mac/AudioDriverCoreAudio.h
        mac/AudioDriverCoreAudio.cpp
        mac/MidiDriverCoreMidi.h
        mac/MidiDriverCoreMidi.cpp

        # The Cocoa editor frame, which is the same one the GMPI AU and VST3
        # wrappers embed in a DAW. mac/ToplevelWindowMac only supplies the
        # window it lives inside.
        ${GMPI_UI_SDK}/backends/DrawingFrameCommon.h
        ${GMPI_UI_SDK}/backends/DrawingFrameCommon.cpp
        ${GMPI_UI_SDK}/backends/DrawingFrameMac.h
        ${GMPI_UI_SDK}/backends/DrawingFrameMac.mm
        ${GMPI_UI_SDK}/backends/CocoaGfx.h
        ${GMPI_UI_SDK}/backends/Gfx_base.h
        ${GMPI_UI_SDK}/backends/Bezier.h

        # The menu bar's font. The Windows shell borrows the frame's own
        # factory; here the frame is an NSView and its factory is private to
        # gmpi_ui, so the app mints one of its own through this helper - see the
        # note in mac/MainMac.mm for why that is equivalent.
        ${GMPI_UI_SDK}/helpers/DrawingFactory.h
        ${GMPI_UI_SDK}/helpers/DrawingFactory_mac.mm
    )

    if(GMPI_STANDALONE_COMMAND_CHANNEL)
        list(APPEND standalone_srcs
            ${standalone_mcp_srcs}     # the transport is the unix socket in IpcServer.h
            mac/FrameCapture.h         # --screenshot: draw the view into our own bitmap
            mac/FrameCapture.mm
        )
    endif()

    foreach(framework
        Cocoa               # the window, the menu bar, the run loop
        CoreFoundation
        CoreGraphics        # the drawing backend
        CoreText            # and its text
        ImageIO             # PNG encode, for --screenshot
        QuartzCore
        Accelerate          # gmpi_ui's image paths
        CoreAudio           # device enumeration
        AudioToolbox        # AUHAL
        AudioUnit
        CoreMIDI            # MIDI input
    )
        find_library(FRAMEWORK_${framework} ${framework})
        mark_as_advanced(FRAMEWORK_${framework})
        list(APPEND standalone_libs ${FRAMEWORK_${framework}})
    endforeach()

    # WEAK, and the header that needs it says why: gmpi_ui's MacFileDialog
    # prefers UTType / allowedContentTypes and falls back to allowedFileTypes
    # below macOS 11, guarded by @available. The framework does not exist on
    # 10.x, so a hard link would stop the app starting at all rather than
    # taking that fallback.
    #
    # The AU, VST3 and CLAP wrappers never had to name this: they are loaded
    # into a host that has already linked it. A standalone is the first target
    # here that links its own executable.
    list(APPEND standalone_libs "-weak_framework UniformTypeIdentifiers")
endif()

# ---------------------------------------------------------------------------
# Linux: Wayland, PipeWire, ALSA.
#
# This arm's dependencies are DISCOVERED, not REQUIRED, and a missing one skips
# the target rather than failing the configure.
#
# The parent's add_subdirectory() promises "safe to add unconditionally", and
# with REQUIRED probes that promise was false: on a Linux box without the
# pipewire SDK the whole tree stopped configuring, so nothing at all could be
# built -- no VST3, no CLAP, no plugin of any kind -- over one optional
# bare-app target.
#
# A hard dependency is the right call for something a product ships; this is a
# developer convenience, so the cost of stopping every other target is far
# larger than the cost of quietly not building it. See TideSynth issue #53.
#
# Declining is not free any more, though: the GMPI SDK's STANDALONE format links
# this target, and an executable that links a target nobody created is only a
# raw linker flag as far as CMake is concerned -- the configure passes and the
# default build then fails. That is why the probe lives in dependencies.cmake
# rather than inline here: gmpi_plugin.cmake includes the same file and drops
# the STANDALONE format when this arm is going to decline.
#
# Set GMPI_STANDALONE_STRICT=ON to turn the skip back into a hard error --
# for CI, or for a developer who means to build this and wants to be told why
# it is not happening rather than reading configure output for it.
# ---------------------------------------------------------------------------

if(UNIX AND NOT APPLE)
option(GMPI_STANDALONE_STRICT
    "Fail the configure if Standalone_Wrapper's dependencies are missing, rather than skipping it" OFF)

# Not OPTIONAL, unlike gmpi_plugin.cmake's include of the same file: that one
# reaches across repos and has to tolerate a wrapper tree older than the probe,
# whereas this is a sibling file and its absence means a broken checkout.
include("${CMAKE_CURRENT_LIST_DIR}/dependencies.cmake")

# The single gate. The include above only probed; nothing below runs unless the
# whole set is present.
if(GMPI_STANDALONE_MISSING_DEPENDENCIES)
    list(JOIN GMPI_STANDALONE_MISSING_DEPENDENCIES "\n    " _standalone_missing_pretty)

    # The Debian/Ubuntu packages, and the only copy of them: README.md's
    # "Building on Linux" points here rather than repeating the line, because a
    # configure message has to be complete at a terminal where nothing else is
    # open -- it is the one that cannot be the pointer.
    #
    # The whole set, not only what is missing: apt does not mind being told
    # about packages already installed, and one line pasted unedited is worth
    # more than a shorter one assembled per machine. Every name answers
    # something dependencies.cmake can report -- libwayland-bin is where
    # wayland-scanner lives, and wayland-protocols supplies both the XML files
    # and the .pc file whose pkgdatadir they are looked for under -- so a probe
    # added there needs its package added here.
    #
    # libdecor-0-plugin-1-gtk is the one name no probe can report: libdecor
    # loads it at run time, so its absence costs the window its title bar and
    # border rather than costing the build anything. Listed anyway, so that
    # pasting this once is enough.
    string(CONCAT _standalone_apt
        "sudo apt install pkg-config "
        "libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev "
        "libdecor-0-dev libdecor-0-plugin-1-gtk libdbus-1-dev "
        "libfontconfig1-dev libharfbuzz-dev libfreetype-dev libpng-dev "
        "libasound2-dev libpipewire-0.3-dev")

    if(GMPI_STANDALONE_STRICT)
        message(FATAL_ERROR
            "Standalone_Wrapper: GMPI_STANDALONE_STRICT is ON and these dependencies are missing:\n"
            "    ${_standalone_missing_pretty}\n"
            "On Debian/Ubuntu the whole set is: ${_standalone_apt}")
    endif()
    message(STATUS
        "Standalone_Wrapper: skipped -- missing:\n"
        "    ${_standalone_missing_pretty}\n"
        "  Every other target still builds. On Debian/Ubuntu the whole set is: "
        "${_standalone_apt}\n"
        "  Set GMPI_STANDALONE_STRICT=ON to make this fatal.")
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

list(APPEND standalone_srcs
    # Linux shell (the entry point itself is added to the executable).
    linux/AudioDriverPipeWire.h
    linux/AudioDriverPipeWire.cpp
    linux/MidiDriverAlsa.h
    linux/MidiDriverAlsa.cpp

    ${WL_PROTOCOL_SOURCES}
)

# The command channel's transport here is the unix socket inside IpcServer.h
# itself, so there is no extra file to add - only the shared half.
if(GMPI_STANDALONE_COMMAND_CHANNEL)
    list(APPEND standalone_srcs ${standalone_mcp_srcs})
endif()

list(APPEND standalone_include_dirs
    ${WL_GEN}
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

list(APPEND standalone_libs
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
list(APPEND standalone_library_dirs
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
endif() # UNIX AND NOT APPLE

# ---------------------------------------------------------------------------
# The target.
# ---------------------------------------------------------------------------

add_library(${PROJECT_NAME} STATIC ${standalone_srcs})

# PUBLIC so the executable that links this gets the SDK include paths (and, on
# Linux, the generated protocol headers) without repeating them - it compiles
# the platform's main() itself, which includes both.
target_include_directories(${PROJECT_NAME} PUBLIC ${standalone_include_dirs})

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

# PUBLIC: the executable compiles the platform's main() itself, and that is
# where the channel is started and stopped. Always defined explicitly, to 0 as
# well as to 1, so the two halves of the app cannot disagree about it - the
# default in CommandChannel.h is for translation units built outside CMake.
target_compile_definitions(${PROJECT_NAME} PUBLIC
    GMPI_STANDALONE_COMMAND_CHANNEL=$<IF:$<BOOL:${GMPI_STANDALONE_COMMAND_CHANNEL}>,1,0>)

if(WIN32)
    # PUBLIC: the executable compiles windows/MainWin32.cpp itself, and it calls
    # the same wide-character entry points this library does.
    target_compile_definitions(${PROJECT_NAME} PUBLIC UNICODE _UNICODE)
endif()

if(APPLE)
    # PUBLIC, because mac/MainMac.mm is compiled by the EXECUTABLE and reaches
    # the same two APIs this library does:
    #
    #  * MIDIInputPortCreate, deprecated since macOS 11 in favour of the UMP
    #    port. Deliberate - see the note at the top of mac/MidiDriverCoreMidi.h -
    #    and suppressed at the call site rather than here. What this covers is
    #    gmpi_ui's Cocoa backend, which is full of the same warnings from
    #    CoreGraphics and AppKit APIs whose replacements postdate the
    #    deployment target. Same flag the AU wrapper carries.
    #  * the enum-to-enum arithmetic CoreAudio's bitmask constants make
    #    unavoidable under C++20.
    target_compile_options(${PROJECT_NAME} PUBLIC
        -Wno-deprecated-declarations
        -Wno-deprecated-enum-enum-conversion
    )
endif()

target_link_libraries(${PROJECT_NAME} PUBLIC ${standalone_libs})

if(standalone_library_dirs)
    target_link_directories(${PROJECT_NAME} PUBLIC ${standalone_library_dirs})
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "wrappers")
