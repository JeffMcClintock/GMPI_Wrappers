#pragma once

// Whether this build has a command channel.
//
// ON by default, and that is the interesting decision, so here is the reason:
// the channel is how a plugin's GUI is testable at all. Nothing else can set a
// parameter and screenshot the result on the app the user is actually looking
// at, and a facility that has to be switched on before it can be used is a
// facility nobody remembers exists. It costs one listener thread that spends
// its life blocked, and no work at all until something connects.
//
// Switch it off with -DGMPI_STANDALONE_COMMAND_CHANNEL=0 (or the CMake option
// GMPI_STANDALONE_COMMAND_CHANNEL=OFF, which sets exactly that). Reasons a
// shipping build might: a plugin vendor who does not want a local IPC endpoint
// in a signed product, or a platform where the transport has not been written.
// With it off, mcp/ is not compiled at all - the switch removes the code rather
// than merely declining to start it, so there is nothing left to audit.
//
// Defaulted HERE rather than only in the CMakeLists so that a translation unit
// compiled outside this project's build - an IDE's syntax pass, a one-off
// compile to reproduce something - behaves the same way the real build does.

#ifndef GMPI_STANDALONE_COMMAND_CHANNEL
#define GMPI_STANDALONE_COMMAND_CHANNEL 1
#endif
