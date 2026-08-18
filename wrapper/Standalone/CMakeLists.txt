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
    # Host-side scaffolding, shared with the VST3/CLAP/AU wrappers - one list,
    # in wrapper/CMakeLists.txt.
    ${GMPI_HOSTING_SRCS}

    # Portable half of the app. StandaloneApp is the startup sequence itself -
    # everything the three shells used to hold a copy of each - so it belongs
    # here beside the views rather than in any arm below.
    AudioMidiDevices.h

    # Here rather than in the three platform arms, even though its only callers
    # are the three MIDI drivers: it is what makes them hand the plugin the same
    # bytes, and a copy per arm is three copies to keep true.
    MidiStreamParser.h

    StandaloneApp.h
    StandaloneApp.cpp
    StandaloneHost.h
    StandaloneHost.cpp
    StandaloneSettings.h
    StandaloneSettings.cpp
    SessionState.h
    SessionState.cpp
    AppLayout.h
    AppLayout.cpp
    MenuBarView.h
    MenuBarView.cpp
    SettingsPane.h
    SettingsPane.cpp

    # The one mcp/ file the portable half names. StandaloneApp.cpp includes it
    # with no #if, and can, because with the channel off its whole body is three
    # inline no-ops; everything behind it is in standalone_mcp_srcs below, which
    # an OFF build never compiles.
    mcp/CommandChannelHost.h

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
# OFF removes the code rather than merely declining to start it: no mcp/
# IMPLEMENTATION is added to the source list below, so a vendor who does not
# want a local IPC endpoint in a signed product has no transport, no dispatcher
# and no listening thread to audit. What an OFF build still compiles is exactly
# one mcp/ file - CommandChannelHost.h, which the portable half includes
# unconditionally and which is three inline no-ops in that configuration.
option(GMPI_STANDALONE_COMMAND_CHANNEL
    "Build the standalone app's command channel (IPC for tests and the MCP server)" ON)

# The portable half of the channel. The transport is per-platform and is added
# by the arms below; everything here is the same code on both.
#
# CommandChannelHost.cpp is in THIS list rather than in the portable one above,
# and that is the whole reason the class exists: it is the only file that names
# IpcServer, whose Windows transport pulls a bare <windows.h> in behind it. Its
# header stays portable and is listed above - see there.
set(standalone_mcp_srcs
    mcp/MainThreadQueue.h
    mcp/CommandProtocol.h     # the wire format, shared by both transports
    mcp/IpcServer.h
    mcp/CommandChannelHost.cpp
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

    # The same helper the VST3, CLAP and AU wrappers use. It also fills in the
    # ${COCOA_LIBRARY}-style cache entries that gmpi_target() links by name,
    # which the FRAMEWORK_<name> spelling this replaced did not - so a project
    # that builds ONLY a standalone now supplies them too, instead of relying on
    # a sibling wrapper having been configured first.
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GmpiFrameworks.cmake")

    gmpi_find_frameworks(standalone_frameworks
        Cocoa               # the window, the menu bar, the run loop
        CoreFoundation
        CoreGraphics        # the drawing backend
        CoreText            # and its text
        ImageIO             # PNG encode, for --screenshot
        QuartzCore
        OpenGL              # not used here, but gmpi_target() links ${OPENGL_LIBRARY}
                            # by name, and a standalone-only project has no wrapper
                            # in its link interface to supply it
        Accelerate          # gmpi_ui's image paths
        CoreAudio           # device enumeration
        AudioToolbox        # AUHAL
        AudioUnit
        CoreMIDI            # MIDI input
    )
    list(APPEND standalone_libs ${standalone_frameworks})

    # The weakly-linked half of the set - gmpi_weak_frameworks() carries the
    # reason. Shared with the other three wrappers rather than spelled out here:
    # the claim this comment used to make - that a wrapper is loaded into a host
    # that has already linked UniformTypeIdentifiers, so only a standalone need
    # name it - is not how linking works. A VST3 module is linked, not merely
    # loaded, and every one of them failed on the undefined _OBJC_CLASS_$_UTType
    # until the wrappers named the flag too.
    gmpi_weak_frameworks(standalone_weak_frameworks)
    list(APPEND standalone_libs ${standalone_weak_frameworks})
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

# The protocol bindings, generated in THIS directory so that the custom commands
# belong to the target below. dependencies.cmake did the resolving half, up at
# the gate; the VST3 wrapper embeds the same gmpi_ui Wayland backend and makes
# the same call for itself.
#
# Included again rather than leaning on dependencies.cmake having done it: a
# file that calls these functions should name where they come from, and the
# module's include_guard makes the second include cost nothing.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/GmpiWayland.cmake")

gmpi_wayland_protocols(WL_PROTOCOL_SOURCES WL_GEN _wl_protocol_missing)

# Cannot fire: the gate above already returned unless the same resolve came back
# clean. Checked anyway because they are two separate calls, and the symptom of
# them disagreeing would be an empty source list - which surfaces much later, as
# a missing xdg-shell-client-protocol.h while the Wayland backend compiles,
# rather than as the missing dependency it is.
if(_wl_protocol_missing)
    message(FATAL_ERROR
        "Standalone_Wrapper: the dependency probe found every Wayland protocol "
        "but code generation did not:\n    ${_wl_protocol_missing}")
endif()

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
