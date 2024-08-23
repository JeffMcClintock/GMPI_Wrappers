#pragma once

#include "pluginterfaces/gui/iplugview.h"
#include "base/source/fobject.h"
#include "GmpiSdkCommon.h"
#include "GmpiApiEditor.h"
#include "helpers/NativeUi.h"

struct pluginInfoSem;

namespace wrapper
{
class VST3Controller;

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
	gmpi::ReturnCode setParameter(int32_t parameterHandle, gmpi::Field fieldId, int32_t voice, int32_t size, const void* data) override;
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
	gmpi::ReturnCode setPin(int32_t pinId, int32_t voice, int32_t size, const void* data) override;
	int32_t getHandle() override;

	gmpi::ReturnCode queryInterface(const gmpi::api::Guid* iid, void** returnInterface) override
	{
		GMPI_QUERYINTERFACE(gmpi::api::IEditorHost);
		GMPI_QUERYINTERFACE(gmpi::api::IParameterObserver);
		return gmpi::ReturnCode::NoSupport;
	}
	GMPI_REFCOUNT;
};

class VST3EditorBase : public Steinberg::FObject, public Steinberg::IPlugView
{
	friend class ParameterHelper;

protected:
	pluginInfoSem const& info;
	wrapper::VST3Controller* controller = {};
    int width, height;
    
	gmpi::shared_ptr<gmpi::api::IEditor> pluginParameters_GMPI;
	gmpi::shared_ptr<gmpi::api::IDrawingClient> pluginGraphics_GMPI;
	ParameterHelper helper;

public:
	VST3EditorBase(pluginInfoSem const& info, gmpi::shared_ptr<gmpi::api::IEditor>& peditor, wrapper::VST3Controller* pcontroller, int pwidth, int pheight);
	~VST3EditorBase();

    void initPlugin();//gmpi::api::IUnknown* host);
	void onParameterUpdate(int32_t parameterHandle, gmpi::Field fieldId, int32_t voice, const void* data, int32_t size);
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
