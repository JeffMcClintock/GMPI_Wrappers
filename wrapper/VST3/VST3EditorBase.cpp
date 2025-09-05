#include "VST3EditorBase.h"
#include "adelaycontroller.h"
#include "MyVstPluginFactory.h"

namespace wrapper
{

ParameterHelper::ParameterHelper(VST3EditorBase* editor)
{
	editor_ = editor;
}

gmpi::ReturnCode ParameterHelper::setParameter(int32_t parameterHandle, gmpi::Field fieldId, int32_t voice, int32_t size, const uint8_t* data)
{
	editor_->onParameterUpdate(parameterHandle, fieldId, voice, data, size);
    return gmpi::ReturnCode::Ok;
}

// GMPI Editor sending a parameter update back to the wrapper.
gmpi::ReturnCode ParameterHelper::setPin(int32_t pinId, int32_t voice, int32_t size, const uint8_t* data)
{
	editor_->controller->gmpiController.setPinFromUi(pinId, voice, { (std::byte*) data, (std::byte*) data + size });
    return gmpi::ReturnCode::Ok;
}

int32_t ParameterHelper::getHandle()
{
    return 0;
}
#if 0

gmpi::ReturnCode ParameterHelper::setCapture()
{
	return gmpi::ReturnCode::Ok;
}
gmpi::ReturnCode ParameterHelper::getCapture(bool& returnValue)
{
	return gmpi::ReturnCode::Ok;
}
gmpi::ReturnCode ParameterHelper::releaseCapture()
{
	return gmpi::ReturnCode::Ok;
}
gmpi::ReturnCode ParameterHelper::getFocus()
{
	return gmpi::ReturnCode::NoSupport;
}
gmpi::ReturnCode ParameterHelper::releaseFocus()
{
	return gmpi::ReturnCode::NoSupport;
}
gmpi::ReturnCode ParameterHelper::getDrawingFactory(gmpi::api::IUnknown** returnFactory)
{
	return gmpi::ReturnCode::Ok;
}
void ParameterHelper::invalidateRect(const gmpi::drawing::Rect* invalidRect)
{
}
#endif

// TODO !!! pass IUnknown to constructor, then QueryInterface for IDrawingClient
VST3EditorBase::VST3EditorBase(gmpi::hosting::pluginInfo const& info, gmpi::shared_ptr<gmpi::api::IEditor>& peditor, wrapper::VST3Controller* pcontroller, int pwidth, int pheight) :
	controller(pcontroller)
	, width(pwidth)
	, height(pheight)
	, pluginParameters_GMPI(peditor)
	, helper(this)
	, info(info)
{
	pluginGraphics_GMPI = pluginParameters_GMPI.as<gmpi::api::IDrawingClient>();
}

void VST3EditorBase::initPlugin(/*gmpi::api::IUnknown* host*/)
{
	controller->gmpiController.registerGui(&helper);

#if 0
	controller->RegisterGui2(&helper);
/*move
	if (pluginParameters_GMPI)
	{
		pluginParameters_GMPI->setHost(host);
		pluginParameters_GMPI->initialize();
	}
 */

	for (auto& p : info.guiPins)
	{
		if (p.parameterId != -1)
		{
			int32_t paramHandle{ -1 };
			controller->getParameterHandle(p.parameterId, paramHandle);
			controller->initializeGui(&helper, paramHandle, p.parameterFieldType);
		}
	}
#endif
}

VST3EditorBase::~VST3EditorBase()
{
#if 0
	controller->UnRegisterGui2(&helper);
#endif
}

void VST3EditorBase::onParameterUpdate(int32_t parameterHandle, gmpi::Field fieldId, int32_t voice, const uint8_t* data, int32_t size)
{
	if (!pluginParameters_GMPI)
		return;

	/* redundant for a single module?
	int32_t moduleHandle{-2};
	int32_t moduleParameterId{-2};
	controller->getParameterModuleAndParamId(parameterHandle, &moduleHandle, &moduleParameterId);
	*/

	//if (-1 == moduleHandle) // not referenced by the GMPI plugin. e.g. HC_PROGRAM_MODIFIED
	//	return;

	for (const auto& pin : info.guiPins)
	{
		if (pin.parameterId == parameterHandle/*moduleParameterId*/ && pin.parameterFieldType == fieldId)
		{
			pluginParameters_GMPI->setPin(pin.id, voice, size, data);
			pluginParameters_GMPI->notifyPin(pin.id, voice);
			break;
		}
	}
}

}
