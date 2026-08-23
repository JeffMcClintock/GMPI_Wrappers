#pragma once

//#include "clap/helpers/plugin.hh"
#include "NativeUi.h"
#ifdef _WIN32
#include "backends/DrawingFrameWin.h"
#endif
#if defined(__linux__)
// BACKLOG S43(ii). __linux__ rather than this file's IS_LINUX, which is defined
// nowhere in the repo -- see the note in Editor_CLAP.cpp.
#include "backends/DrawingFrameX11.h"
#endif

namespace gmpi { namespace hosting
{

struct Editor_CLAP
{
#if _WIN32
	gmpi::hosting::DrawingFrame drawingframe;
#endif
    
#if __APPLE__
    void* nsView{};
#endif

#if defined(__linux__)
    // Same frame the VST3 wrapper embeds through (wrapper/VST3/SEVSTGUIEditorLinux).
    // It runs no event loop of its own: the host polls connectionFd() and calls
    // processEvents(), and ticks onTimer(). CLAP supplies both through its
    // posix-fd and timer host extensions, which is exactly the pairing
    // guiIsApiSupported() checks for before claiming X11.
    gmpi::hosting::X11DrawingFrame drawingframe;

    // The clap_id the host gave us for the repaint timer, and the X connection
    // fd we registered. Kept so teardown can unregister exactly what it
    // registered -- see the ordering note in Processor_CLAP::guiDestroy.
    bool hasTimer{};
    uint32_t timerId{};
    int registeredFd{ -1 };

    // Did the host choose the embed size, or must we use our own measured
    // preference? A CLAP host that accepts get_size()'s answer never calls
    // set_size, and width/height then stay at their {100} defaults -- which
    // is what embedded a 100x100 editor into a 1100x600 window.
    bool sizeSetByHost{};
#endif
    
	gmpi::shared_ptr<gmpi::api::IEditor> pluginParameters_GMPI;
	gmpi::shared_ptr<gmpi::api::IDrawingClient> pluginGraphics_GMPI;
	struct gmpi_controller_holder* gmpiController{};

	float Dpi{ 1.0f };

//	HWND myhwnd{};
	uint32_t width{ 100 };
	uint32_t height{ 100 };
//	gmpi::shared_ptr<gmpi::api::IDrawingClient> client;

	Editor_CLAP(gmpi_controller_holder* gmpiController);
	~Editor_CLAP();

	void open(void* parentWindow);

	//HWND getWindowHandle() override
	//{
	//	return myhwnd;
	//}
	//float calcWhiteLevel() override { return 1.0f; }

	void getSize(uint32_t& width, uint32_t& height);
	void setSize(uint32_t width, uint32_t height);
	//	bool onTimer() override { return true };
};

}}
