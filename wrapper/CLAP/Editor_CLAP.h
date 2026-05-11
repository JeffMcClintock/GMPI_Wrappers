#pragma once

//#include "clap/helpers/plugin.hh"
#include "NativeUi.h"
#ifdef _WIN32
#include "backends/DrawingFrameWin.h"
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
