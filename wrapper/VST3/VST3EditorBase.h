#pragma once

#include "pluginterfaces/gui/iplugview.h"
#include "base/source/fobject.h"
#include "GmpiSdkCommon.h"
#include "GmpiApiEditor.h"
#include "helpers/NativeUi.h"
#include "Hosting/xml_spec_reader.h"

namespace wrapper
{
class Controller_VST3;

#if 0
class ParameterHelper :
	public gmpi::api::IParameterObserver,
	// AH!!!!!, already in gmpi::hosting::DrawingFrame !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	// probly need to redirect queryInterface from here to drawing frame or vica versa
	//public gmpi::api::IInputHost,
	public gmpi::api::IEditorHost
	//public gmpi::api::IDrawingHost
{
	class VST3EditorBase* editor_ = {};

public:
	ParameterHelper(class VST3EditorBase* editor);

	//---IParameterObserver------
	gmpi::ReturnCode setParameter(int32_t parameterHandle, gmpi::Field fieldId, int32_t voice, int32_t size, const uint8_t* data) override;
#if 0
	// IInputHost
	gmpi::ReturnCode setCapture() override;
	gmpi::ReturnCode getCapture(bool& returnValue) override;
	gmpi::ReturnCode releaseCapture() override;
	gmpi::ReturnCode getFocus() override;
	gmpi::ReturnCode releaseFocus() override;
	// IDrawingHost
	gmpi::ReturnCode getDrawingFactory(gmpi::api::IUnknown** returnFactory) override;
	void invalidateRect(const gmpi::drawing::Rect* invalidRect) override;
#endif
	//---IEditorHost------
	gmpi::ReturnCode setPin(int32_t pinId, int32_t voice, int32_t size, const uint8_t* data) override;
	int32_t getHandle() override;

	gmpi::ReturnCode queryInterface(const gmpi::api::Guid* iid, void** returnInterface) override
	{
		GMPI_QUERYINTERFACE(gmpi::api::IEditorHost);
		GMPI_QUERYINTERFACE(gmpi::api::IParameterObserver);
		return gmpi::ReturnCode::NoSupport;
	}
	GMPI_REFCOUNT;
};
#endif

// The availableSize handed to IDrawingClient::measure when asking "how big
// would you like to be?". Effectively unbounded: there is no real window yet.
inline constexpr float kUnboundedMeasure = 99999.f;

// Ask a client for its preferred size, in DIPs, updating width/height only if
// it actually expressed a preference.
//
// measure() answers "given at most this much room, how much do you want?", so
// against an unbounded offer there are two legitimate answers:
//
//   fixed-size client  -> its size. Use it.
//   resizable client   -> the offer, echoed straight back, meaning "anything".
//                         canResize() below depends on exactly that behaviour,
//                         and it is what PluginEditor::measure does by default.
//
// The second is not a size. Taking it literally asks for a 99999 x 99999 window;
// DXGI refuses to create a swap chain that large and the frame dies inside
// CreateSwapPanel, with a stack that mentions only graphics and never reveals
// the offending number. So an unbounded answer keeps the caller's default, and
// anything absurd is treated the same way rather than trusted.
inline void measurePreferredSize(gmpi::api::IDrawingClient* client, float dpi, int& width, int& height)
{
	if (!client)
		return;

	const gmpi::drawing::Size availableSize{ kUnboundedMeasure, kUnboundedMeasure };
	gmpi::drawing::Size desiredSize{ static_cast<float>(width), static_cast<float>(height) };

	if (client->measure(&availableSize, &desiredSize) != gmpi::ReturnCode::Ok)
		return;

	// Below 1 DIP is not a size either — a zero-size window breaks the same
	// swap-chain call from the other direction.
	const bool expressedPreference =
		   desiredSize.width  >= 1.0f && desiredSize.width  < kUnboundedMeasure
		&& desiredSize.height >= 1.0f && desiredSize.height < kUnboundedMeasure;

	if (!expressedPreference)
		return; // keep the caller's default

	width  = static_cast<int>(dpi * desiredSize.width);
	height = static_cast<int>(dpi * desiredSize.height);
}

class VST3EditorBase : public Steinberg::FObject, public Steinberg::IPlugView
{
	friend class ParameterHelper;

protected:
	gmpi::hosting::pluginInfo const& info;
	wrapper::Controller_VST3* controller = {};
    int width, height;
    
	gmpi::shared_ptr<gmpi::api::IEditor> pluginParameters_GMPI;
	gmpi::shared_ptr<gmpi::api::IDrawingClient> pluginGraphics_GMPI;

public:
	VST3EditorBase(gmpi::hosting::pluginInfo const& info, gmpi::shared_ptr<gmpi::api::IEditor>& peditor, wrapper::Controller_VST3* pcontroller, int pwidth, int pheight);
	~VST3EditorBase();

    void initPlugin();//gmpi::api::IUnknown* host);
	void onParameterUpdate(int32_t parameterHandle, gmpi::Field fieldId, int32_t voice, const uint8_t* data, int32_t size);
#if 0
	//---from IPlugView-------
	Steinberg::tresult PLUGIN_API isPlatformTypeSupported (Steinberg::FIDString type) SMTG_OVERRIDE { return Steinberg::kResultTrue; }
	Steinberg::tresult PLUGIN_API attached (void* parent, Steinberg::FIDString type) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API removed () SMTG_OVERRIDE;

	Steinberg::tresult PLUGIN_API onWheel (float /*distance*/) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
	Steinberg::tresult PLUGIN_API onKeyDown (Steinberg::char16 /*key*/, Steinberg::int16 /*keyMsg*/,
	                              Steinberg::int16 /*modifiers*/) SMTG_OVERRIDE {return Steinberg::kResultFalse;}
	Steinberg::tresult PLUGIN_API onKeyUp (Steinberg::char16 /*key*/, Steinberg::int16 /*keyMsg*/, Steinberg::int16 /*modifiers*/) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
	Steinberg::tresult PLUGIN_API getSize (Steinberg::ViewRect* size) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) SMTG_OVERRIDE;

	Steinberg::tresult PLUGIN_API onFocus (Steinberg::TBool /*state*/) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
	Steinberg::tresult PLUGIN_API setFrame (Steinberg::IPlugFrame* frame) SMTG_OVERRIDE	{return Steinberg::kResultTrue;	}

	Steinberg::tresult PLUGIN_API canResize() SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* /*rect*/) SMTG_OVERRIDE;
#endif

	//---Interface------
	OBJ_METHODS (VST3EditorBase, Steinberg::FObject)
	DEFINE_INTERFACES
	DEF_INTERFACE (IPlugView)
	END_DEFINE_INTERFACES (Steinberg::FObject)
	REFCOUNT_METHODS (Steinberg::FObject)
};
}
