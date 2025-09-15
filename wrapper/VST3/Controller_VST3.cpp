#include "base/source/fstring.h"
#include "base/source/updatehandler.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/vst/vstpresetfile.h"
//#include "public.sdk/source/common/memorystream.h"
#include "Controller_VST3.h"
//#include "MyVstPluginFactory.h"
#include "Processor_VST3.h"
#include "GmpiApiEditor.h"
//#include "wrapper/common/se_datatypes.h" // kill this
#include "wrapper/common/unicode_conversion.h"
#include "Hosting/gmpi_factory.h"


#ifdef _WIN32
#include "SEVSTGUIEditorWin.h"
#else
#include "SEVSTGUIEditorMac.h"
#endif


/*
#include "midi_defs.h"
#include "conversion.h"
#include "IPluginGui.h"
#include "HostControls.h"
#include "../Shared/jsoncpp/json/json.h"
#include "modules/shared/FileFinder.h"
#include "UgDatabase.h"
#include "../se_sdk3_hosting/GuiPatchAutomator3.h"
#include "tinyxml/tinyxml.h" // anoyingly defines DEBUG as blank which messes with vstgui. put last.
#include "../tinyXml2/tinyxml2.h"
#include "VstPreset.h"
#include "Vst2Preset.h"

#ifdef _WIN32
#include "SEVSTGUIEditorWin.h"
#include "../../se_vst3/source/MyVstPluginFactory.h"
#include "pluginterfaces/base/funknown.h"
#include "../shared/unicode _conversion.h"
#else
#include "SEVSTGUIEditorMac.h"
#endif
#include "AuPreset.h"
#include "mfc_emulation.h"

using namespace std;
using namespace tinyxml2;
*/
using namespace Steinberg;
using namespace Steinberg::Vst;

typedef gmpi::ReturnCode(*MP_DllEntry)(void**);

namespace wrapper
{

//using namespace JmUnicodeConversions;

#if 0
void Safe Messagebox(
	void* hWnd,
	const wchar_t* lpText,
	const wchar_t* lpCaption = L"",
	int uType = 0
)
{
	_RPTW1(0, L"%s\n", lpText);
}

MpParameterVst3::MpParameterVst3(Controller_VST3* controller, /*int strictIndex, */int ParameterTag, bool isInverted) :
	MpParameter_native({}),//controller),
	vst3Controller(controller),
	isInverted_(isInverted),
	hostTag(ParameterTag)
{
}

void MpParameterVst3::updateProcessor(gmpi::Field fieldId, int32_t voice)
{
	switch (fieldId)
	{
		/* double up
	case gmpi::Field::Grab:
		controller_->ParamGrabbed(this, voice);
		break;
		*/

	case gmpi::Field::Value:
	case gmpi::Field::Normalized:
		vst3Controller->ParamToProcessorViaHost(this, voice);
		break;
	}
}
#endif


Controller_VST3::Controller_VST3(gmpi::hosting::pluginInfo& pinfo) :
//	MpController(pinfo)
	 isInitialised(false)
	, isConnected(false)
{
// using tinxml 1?	TiXmlBase::SetCondenseWhiteSpace(false); // ensure text parameters preserve multiple spaces. e.g. "A     B" (else it collapses to "A B")

#if 0
	// Scan all presets for preset-browser.
	ScanPresets();
#endif

	gmpiController.notifyDaw = [this](gmpi::hosting::GmpiParameter const* param)
		{
			assert(param->info->dawTag != -1); // should never be called for non-native param.

			const auto paramID = param->info->dawTag;

			// Usually parameter will have sent beginEdit() already (if it has mouse-down connected properly, else fake it.
			if (!param->isGrabbed)
				beginEdit(paramID);

			//   _RPT2(0, "param[%d] %f => DAW\n", paramID, param->getNormalized());
			performEdit(paramID, param->normalisedValue()); // Send the value to DSP.

			if (!param->isGrabbed)
				endEdit(paramID);
		};

	gmpiController.init(pinfo);
}

Controller_VST3::~Controller_VST3()
{
#if 1
	stopTimer();
#endif
}

tresult PLUGIN_API Controller_VST3::connect(IConnectionPoint* other)
{
	auto r = EditController::connect(other);

	isConnected = true;

#if 0

	// Can only init controllers after both VST controller initialised AND controller is connected to processor.
	// So VST2 wrapper aeffect pointer makes it to Processor.
	if(isConnected && isInitialised)
		initSemControllers();
#endif

	return r;
}

tresult PLUGIN_API Controller_VST3::notify( IMessage* message )
{
	// WARNING: CALLED FROM PROCESS THREAD (In Ableton Live).
	if( !message )
		return kInvalidArgument;

	if( !strcmp( message->getMessageID(), "BinaryMessage" ) )
	{
		const void* data;
		uint32 size;
		if( message->getAttributes()->getBinary( "MyData", data, size ) == kResultOk )
		{
			gmpiController.message_que_dsp_to_ui.pushString(size, (unsigned char*) data);
			gmpiController.message_que_dsp_to_ui.Send();
			return kResultOk;
		}
	}

	return EditController::notify( message );
}

bool Controller_VST3::sendMessageToProcessor(const void* data, int size)
{
	auto message = allocateMessage();
	if (message)
	{
		FReleaser msgReleaser(message);
		message->setMessageID("BinaryMessage");

		message->getAttributes()->setBinary("MyData", data, size);
		sendMessage(message);
	}

	return true;
}
#if 0

void Controller_VST3::ParamGrabbed(MpParameter_native* param)
{
	auto paramID = param->getNativeTag();

	if (param->isGrabbed())
	{
//		_RPT0(0, "DAW GRAB\n");
		beginEdit(paramID);
	}
	else
	{
//		_RPT0(0, "DAW UN-GRAB\n");
		endEdit(paramID);
	}
}
#endif

#if 0
void Controller_VST3::ParamToProcessorViaHost(MpParameterVst3* param, int32_t voice)
{
	const auto paramID = param->getNativeTag();

	// Usually parameter will have sent beginEdit() already (if it has mouse-down connected properly, else fake it.
	if (!param->isGrabbed())
		beginEdit(paramID);

 //   _RPT2(0, "param[%d] %f => DAW\n", paramID, param->getNormalized());
	performEdit(paramID, param->convertNormalized(param->getNormalized())); // Send the value to DSP.

	if (!param->isGrabbed())
		endEdit(paramID);
}

void Controller_VST3::ResetProcessor()
{
	// Currently called when polyphony etc changes, VST2 wrapper ignores this completely, at least in Live.
//	componentHandler->restartComponent(kLatencyChanged); // or kIoChanged might be less overhead for DAW
}
#endif

enum class ElatencyContraintType
{
	Off, Constrained, Full
};

struct pluginInformation
{
	int32_t pluginId = -1;
	std::string manufacturerId;
	int32_t inputCount;
	int32_t outputCount;
	int32_t midiInputCount;
	ElatencyContraintType latencyConstraint;
	std::string pluginName;
	std::string vendorName;
	std::string subCategories;
	bool outputsAsStereoPairs;
	bool emulateIgnorePC = {};
	bool monoUseOk;
	bool vst3Emulate16ChanCcs;
	std::vector<std::string> outputNames;
};

#if 0
void Controller_VST3::setPinFromUi(int32_t pinId, int32_t voice, int32_t size, const void* data)
{
	for (auto& pin : info.guiPins)
	{
		if (pin.id == pinId && /*pin.direction == gmpi::PinDirection::In && pin.datatype == gmpi::PinDatatype::Float32 &&*/ pin.parameterId != -1)
		{
			for (auto& param : info.parameters)
			{
				if (param.id == pin.parameterId)
				{
					auto param = tagToParameter[pin.parameterId];
					setParameterValue({ data, static_cast<size_t>(size)}, param->parameterHandle_, pin.parameterFieldType, voice);
					break;
				}
			}
			break;
		}
	}
}
#endif

#if 0
// send initial value of all parameters to GUI
void Controller_VST3::initUi(gmpi::api::IParameterObserver* gui)
{
	for (auto& it : tagToParameter)
	{
		initializeGui(gui, it.second->parameterHandle_, gmpi::Field::Value);
	}
}
#endif

//-----------------------------------------------------------------------------
tresult PLUGIN_API Controller_VST3::initialize (FUnknown* context)
{
	UpdateHandler::instance();

	tresult result = EditController::initialize (context);
	if (result == kResultTrue)
	{
		// Modules database, needed here to load any controllers. e.g. VST2 child plugins.
		{
//?			ModuleFactory()->RegisterExternalPluginsXmlOnce(nullptr);
		}

//		MpController::Initialize();

		supportedChannels = 1; // TODO countPins(info, gmpi::PinDirection::In, gmpi::PinDatatype::Midi) == 0 ? 0 : 16;

//		const auto supportedChannels = info.midiInputCount ? 16 : 0;
		const auto supportAllCC = true; // info.vst3Emulate16ChanCcs;

#if 0

		// MIDI CC SUPPORT
		char ccName[] = "MIDI CC     ";
		for (int chan = 0; chan < supportedChannels; ++chan)
		{
			for (int cc = 0; cc < numMidiControllers; ++cc)
			{
				if (chan == 0 || supportAllCC || cc > 127 || cc == 74) // Channel Presure, Pitch Bend and Brightness
				{
					const ParamID ParameterId = MidiControllersParameterId + chan * numMidiControllers + cc;
					sprintf(ccName + 9, "%3d", cc);

					auto param = makeNativeParameter(ParameterId, false);
					param->datatype_ = gmpi::PinDatatype::Float32;
					param->name_ = ccName;
					param->minimum = 0;
					param->maximum = 1;
					param->hostControl_ = -1;
					param->isPolyphonic_ = false;
					param->parameterHandle_ = -1;
					param->moduleHandle_ = -1;
					param->moduleParamId_ = -1;
					param->stateful_ = false;

					auto initialVal(ParseToRaw(param->datatype_, "0"));
					param->rawValues_.push_back(initialVal);				// preset 0/

					parameters_.push_back(std::unique_ptr<MpParameter>(param));
				}
			}
		}
#endif
	}

	isInitialised = true;

#if 0

	// Can only init controllers after both VST controller initialised AND controller is connected to processor.
	// So VST2 wrapper aeffect pointer makes it to Processor.
	if (isConnected && isInitialised)
		initSemControllers();
#endif

	const int timerPeriodMs = 35;
	startTimer(timerPeriodMs);

	return kResultTrue;
}

// Parameter updated from DAW.
void Controller_VST3::update( FUnknown* changedUnknown, int32 message )
{
	EditController::update( changedUnknown, message );

	if( message == IDependent::kChanged )
	{
	assert(false);
		auto parameter = dynamic_cast<Parameter*>( changedUnknown );
		if( parameter )
		{
			/*
			auto& it = vst3IndexToParameter.find(parameter->getInfo().id);
			if (it != vst3IndexToParameter.end())
			{
				const int voiceId = 0;
				update Guis((*it).second, voiceId);
			}
			*/
		}
	}
}

//------------------------------------------------------------------------
tresult PLUGIN_API Controller_VST3::getMidiControllerAssignment (int32 busIndex, int16 channel, CtrlNumber midiControllerNumber, ParamID& tag/*out*/)
{
#if 0 // TODO

	if (busIndex == 0)
	{
		const int possibleTag = MidiControllersParameterId + channel * numMidiControllers + midiControllerNumber;
		if (tagToParameter.find(possibleTag) != tagToParameter.end())
		{
			tag = possibleTag;
			return kResultTrue;
		}
	}
#endif

	return kResultFalse;
}

IPlugView* PLUGIN_API Controller_VST3::createView (FIDString name)
{
	if (ConstString (name) != ViewType::kEditor)
		return {};
	
	auto info = gmpi::hosting::factory::getInstance().getPluginInfo();

	if (!info)
		return {};

	auto pluginUnknown = gmpi::hosting::factory::getInstance().createInstance(info->id.c_str(), gmpi::api::PluginSubtype::Editor);

	if (!pluginUnknown )
		return {};

	auto editor = pluginUnknown.as<gmpi::api::IEditor>();

	if (!editor)
		return {};

	const int width { 200 };
	const int height{ 200 };
#ifdef _WIN32
	return new SEVSTGUIEditorWin(*info, editor, this, width, height);
#else
	return new SEVSTGUIEditorMac(*info, editor, this, width, height);
#endif
// todo init all params and pins			initializeGui(&helper)
}

// Preset Loaded.
tresult PLUGIN_API Controller_VST3::setComponentState (IBStream* state)
{
	int32 bytesRead;
	int32 chunkSize = 0;
	std::string chunk;
	state->read( &chunkSize, sizeof(chunkSize), &bytesRead );
	chunk.resize(chunkSize);
	state->read((void*) chunk.data(), chunkSize, &bytesRead);

#if 0
	DawPreset preset(parametersInfo, chunk);
	setPreset(&preset);
#endif
	gmpiController.setPresetXmlFromDaw(chunk);

	return kResultTrue;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Controller_VST3::queryInterface (const char* iid, void** obj)
{
	QUERY_INTERFACE(iid, obj, IMidiMapping::iid, IMidiMapping)
	QUERY_INTERFACE(iid, obj, INoteExpressionController::iid, INoteExpressionController)
	QUERY_INTERFACE(iid, obj, INoteExpressionPhysicalUIMapping::iid, INoteExpressionPhysicalUIMapping)

	return EditController::queryInterface (iid, obj);
}

tresult Controller_VST3::setParamNormalized( ParamID tag, ParamValue value )
{
//	_RPT2(_CRT_WARN, "setParamNormalized(%d, %f)\n", tag, value);

	if (auto p = gmpiController.patchManager.setParameterNormalised(gmpiController.nativeParams[tag]->info->id, value); p) // todo avoid lookup when we already have pointer to parameter
	{
		gmpiController.notifyGui(p);
	}

	return kResultTrue;
}
#if 0

void Controller_VST3::OnLatencyChanged()
{
	getComponentHandler()->restartComponent(kLatencyChanged);
//	_RPT0(_CRT_WARN, "restartComponent(kLatencyChanged)\n");
}
#endif

tresult Controller_VST3::getParameterInfo(int32 paramIndex, ParameterInfo& returnInfo)
{
	if( paramIndex < 0 || paramIndex >= static_cast<int>(gmpiController.nativeParams.size()))
		return kInvalidArgument;

	const auto& p = *gmpiController.nativeParams[paramIndex];

	returnInfo.flags = Steinberg::Vst::ParameterInfo::kCanAutomate;
	returnInfo.defaultNormalizedValue = 0.0;

	auto temp = JmUnicodeConversions::ToUtf16(p.info->name);

	_tstrncpy(returnInfo.shortTitle, (const TChar*)temp.c_str(), static_cast<Steinberg::uint32>(std::size(returnInfo.shortTitle)));
	_tstrncpy(returnInfo.title     , (const TChar*)temp.c_str(), static_cast<Steinberg::uint32>(std::size(returnInfo.title)));

	returnInfo.id = paramIndex;// p.getNativeTag();
	returnInfo.unitId = kRootUnitId;
	returnInfo.stepCount = 0;
	returnInfo.units[0] = 0;

	if ((p.info->datatype == gmpi::PinDatatype::Int32 || p.info->datatype == gmpi::PinDatatype::Int64) && !p.info->enum_entries.empty())
	{
		returnInfo.flags |= Steinberg::Vst::ParameterInfo::kIsList;
		returnInfo.stepCount = (std::max)(0, static_cast<int>(p.info->enum_entries.size()) - 1);
	}

	// Support for VSTs special bypass parameter.
	if (p.info->hostConnect == gmpi::hosting::HostControls::ProcessBypass)
	{
		returnInfo.flags |= Steinberg::Vst::ParameterInfo::kIsBypass;
	}

	return kResultOk;
}

tresult PLUGIN_API Controller_VST3::getParamStringByValue(ParamID tag, ParamValue valueNormalized, String128 string)
{
	if (tag < 0 || tag >= static_cast<int>(gmpiController.nativeParams.size()))
		return 0.0;

	const auto& p = *gmpiController.nativeParams[tag];

	// enums
	if ((p.info->datatype == gmpi::PinDatatype::Int32 || p.info->datatype == gmpi::PinDatatype::Int64) && !p.info->enum_entries.empty())
	{
		const int index = std::clamp(static_cast<int>(std::round(p.normalized2Real(valueNormalized))), 0, static_cast<int>(p.info->enum_entries.size()) - 1);
		auto s = JmUnicodeConversions::ToUtf16(p.info->enum_entries[index].name);
		strncpy16(string, (const TChar*)s.c_str(), 128);
		return kResultOk;
	}

	const auto valueString = std::to_wstring(p.normalized2Real(valueNormalized));
	const auto s_UTF16 = JmUnicodeConversions::ToUtf16(valueString);
	strncpy16(string, (const TChar*)s_UTF16.c_str(), 128);
	return kResultOk;
}

#if 0 // TODO
std::string Controller_VST3::loadNativePreset(std::wstring sourceFilename)
{
	auto filetype = GetExtension(sourceFilename);

	if (filetype == L"vstpreset")
	{
		return VstPresetUtil::ReadPreset(sourceFilename);
	}

	if (filetype == L"aupreset")
	{
		return AuPresetUtil::ReadPreset(sourceFilename);
	}

	if (filetype == L"fxp")
	{
		auto factory = MyVstPluginFactory::GetInstance();
		const int pluginIndex = 0;
		const auto xml = Vst2PresetUtil::ReadPreset(ToPlatformString(sourceFilename), factory->getVst2Id(pluginIndex), factory->getVst2Id64(pluginIndex));

		if(!xml.empty())
		{
			return xml;
		}

		// Prior to v1.4.518 - 2020-08-04, Preset Browser saves VST3 format instead of VST2 format. Fallback to VST3.
		return VstPresetUtil::ReadPreset(sourceFilename);
	}
	return {};
}
#endif

#if 0 // todo
std::vector< MpController::presetInfo > Controller_VST3::scanFactoryPresets()
{
	platform_string factoryPresetsFolder(_T("vst2FactoryPresets/"));
	auto factoryPresetFolder = ToPlatformString(BundleInfo::instance()->getImbeddedFileFolder());
	auto fullpath = combinePathAndFile(factoryPresetFolder, factoryPresetsFolder);

	if (fullpath.empty())
	{
		return {};
	}

	return scanPresetFolder(fullpath, _T("xmlpreset"));
	return {};
}
#endif

#if 0 // TODO
void Controller_VST3::loadFactoryPreset(int index, bool fromDaw)
{
	platform_string vst2FactoryPresetsFolder(_T("vst2FactoryPresets/"));
	auto PresetFolder = ToPlatformString(BundleInfo::instance()->getImbeddedFileFolder());
	PresetFolder = combinePathAndFile(PresetFolder, vst2FactoryPresetsFolder);

	auto fullFilePath = PresetFolder + ToPlatformString(presets[index].filename);
	auto filenameUtf8 = ToUtf8String(fullFilePath);
	ImportPresetXml(filenameUtf8.c_str());
}

void Controller_VST3::setPresetFromSelf(DawPreset const* preset)
{
	// since there is no explicit sharing between controller and processor, we need to send entire preset in one hit via queue
	const auto xml = preset->toString(BundleInfo::instance()->getPluginId());
	setPresetXmlFromSelf(xml);
}

void Controller_VST3::setPresetXmlFromSelf(const std::string& xml)
{
	// send to processor
	auto message = allocateMessage();
	if (message)
	{
		FReleaser msgReleaser(message);
		message->setMessageID("BinaryMessage");

		message->getAttributes()->setBinary("Preset", xml.data(), static_cast<uint32>(xml.size()));
		sendMessage(message);
	}

	// send to controller
	DawPreset preset(parametersInfo, xml);
	setPreset(&preset);

#if 0
	// since there is not explicit sharing between controller and processor, we need to send entire preset in one hit via queue
	
	const auto freespace = getQueueToDsp()->freeSpace();
	const uint32_t messageSize = static_cast<uint32_t>(sizeof(int32_t) + xml.size());

	if (freespace < messageSize)
	{
		assert(false); // Preset too large for message queue
		return;
	}

	my_msg_que_output_stream s(getQueueToDsp(), UniqueSnowflake::APPLICATION, "PROG"); // Program Change

	s << messageSize;
	s << xml;

	s.Send();
#endif
}
#endif

platform_string Controller_VST3::calcFactoryPresetFolder()
{
	// TODO
	return {};
}
#if 0

std::string Controller_VST3::getFactoryPresetXml(std::string filename)
{
	auto PresetFolder = calcFactoryPresetFolder();
	auto fullFilePath = PresetFolder + ToPlatformString(filename);
	return loadNativePreset(toWstring(fullFilePath));
}
#endif

#if 0
void Controller_VST3::saveNativePreset(const char* filename, const std::string& presetName, const std::string& xml)
{
	const auto filetype = GetExtension(std::string(filename));

	// VST2 preset format.
	if(filetype == "fxp")
	{
		auto factory = MyVstPluginFactory::GetInstance();
		Vst2PresetUtil::WritePreset(ToPlatformString(filename), factory->getVst2Id64(0), presetName, xml);
		return;
	}

	// VST3 preset format.
	{
		// Add Company name.
		auto factory = MyVstPluginFactory::GetInstance();
		auto processorId = factory->getPluginInfo().processorId;

		std::string categoryName; // TODO !!!
		VstPresetUtil::WritePreset(JmUnicodeConversions::Utf8ToWstring(filename), categoryName, factory->getVendorName(), factory->getProductName(), &processorId, xml);
	}
}
#endif

bool Controller_VST3::onTimer()
{
	// parameter updates from the Processor
	gmpiController.message_que_dsp_to_ui.pollMessage(&gmpiController);

	// parameter updates to the Processor
/* TODO, rationalise with clap controller, who should own the queue?
	gmpi::hosting::my_msg_que_output_stream toProcessor(&queueToDsp_);
	gmpiController.pendingControllerQueueClients.ServiceWaiters(
		toProcessor,
		queueToDsp_.freeSpace(),
		queueToDsp_.freeSpace()
	);
*/
	if (!queueToDsp_.empty())
	{
		sendMessageToProcessor(queueToDsp_.data(), queueToDsp_.size());
		queueToDsp_.clear();
	}

	return true; // MpController::onTimer();
}

int32 Controller_VST3::getNoteExpressionCount(int32 busIndex, int16 channel)
{
    // we accept only the first bus and 1 channel
    if (busIndex != 0 || channel != 0)
        return 0;

	return 1;
}

tresult Controller_VST3::getNoteExpressionInfo (int32 busIndex, int16 channel, int32 noteExpressionIndex, NoteExpressionTypeInfo& info)
{
    // we accept only the first bus and 1 channel
    if (busIndex != 0 || channel != 0 || noteExpressionIndex > 3)
        return kResultFalse;

	memset (&info, 0, sizeof (NoteExpressionTypeInfo));
 
	// might be better to support two generic types than volume and pan.
	NoteExpressionTypeID typeIds[] =
	{
		//kVolumeTypeID,
		//kPanTypeID,
		kTuningTypeID,
		kExpressionTypeID,
		kBrightnessTypeID,
	};

	info.typeId = typeIds[noteExpressionIndex];
	info.valueDesc.minimum = 0.0;
	info.valueDesc.maximum = 1.0;
	info.valueDesc.defaultValue = 0.5;
	info.valueDesc.stepCount = 0; // we want continuous (no step)
	info.unitId = -1;
	info.associatedParameterId = -1;

	switch (info.typeId)
	{
	case kVolumeTypeID:
	{
		assert(info.typeId == kVolumeTypeID);

		// set some strings
		USTRING("Volume").copyTo(info.title, 128);
		USTRING("Vol").copyTo(info.shortTitle, 128);
		info.valueDesc.defaultValue = 1.0;
	}
	break;

	case kPanTypeID: // polyphonic pan.
	{
		assert(info.typeId == kPanTypeID);

		// set some strings
		USTRING("Pan").copyTo(info.title, 128);
		USTRING("Pan").copyTo(info.shortTitle, 128);

		info.flags = NoteExpressionTypeInfo::kIsBipolar | NoteExpressionTypeInfo::kIsAbsolute; // event is bipolar (centered)
	}
	break;

	case kTuningTypeID: // polyphonic pitch-bend. ('tuning')
	{
		// set the tuning type
		assert(info.typeId == kTuningTypeID);

		// set some strings
		USTRING("Tuning").copyTo(info.title, 128);
		USTRING("Tun").copyTo(info.shortTitle, 128);
		USTRING("Half Tone").copyTo(info.units, 128);

		info.flags = NoteExpressionTypeInfo::kIsBipolar; // event is bipolar (centered)

		// The range you set here, determins the default range in Cubase Note Expression Inspector.
		// Set it to +- 24 for Roli MPE Controller

		// for Tuning the convert functions are : plain = 240 * (norm - 0.5); norm = plain / 240 + 0.5;
		// we want to support only +/- one octave
		constexpr double kNormTuningOneOctave = 12.0 / 240.0;

		info.valueDesc.minimum = 0.5 - kNormTuningOneOctave;
		info.valueDesc.maximum = 0.5 + kNormTuningOneOctave;
//		info.valueDesc.defaultValue = 0.5; // middle of [0, 1] => no detune (240 * (0.5 - 0.5) = 0)
	}
	break;

	case kBrightnessTypeID:
		USTRING("Brightness").copyTo(info.title, 128);
		USTRING("Brt").copyTo(info.shortTitle, 128);
		info.flags = NoteExpressionTypeInfo::kIsAbsolute;
		break;

	case kExpressionTypeID:
		USTRING("Expression").copyTo(info.title, 128);
		USTRING("Exp").copyTo(info.shortTitle, 128);
		info.flags = NoteExpressionTypeInfo::kIsAbsolute;
		break;

	default:
        return kResultFalse;
	}
		
	return kResultTrue;
}

tresult Controller_VST3::getNoteExpressionStringByValue (int32 busIndex, int16 channel, NoteExpressionTypeID id, NoteExpressionValue valueNormalized , String128 string)
{
    // we accept only the first bus and 1 channel
    if (busIndex != 0 || channel != 0)
        return kResultFalse;

	switch (id)
	{
	case kTuningTypeID: // polyphonic pitch-bend.
	{
		// here we have to convert a normalized value to a Tuning string representation
		UString128 wrapper;
		valueNormalized = (240 * valueNormalized) - 120; // compute half Tones
		wrapper.printFloat (valueNormalized, 2);
		wrapper.copyTo (string, 128);
	}
	break;

	case kPanTypeID: // polyphonic pan.
	{
		// here we have to convert a normalized value to a Tuning string representation
		UString128 wrapper;
		valueNormalized = valueNormalized * 2.0 - 1.0;
		wrapper.printFloat (valueNormalized, 2);
		wrapper.copyTo (string, 128);
	}
	break;

	default:
	{
		// here we have to convert a normalized value to a Tuning string representation
		UString128 wrapper;
		wrapper.printFloat (valueNormalized, 2);
		wrapper.copyTo (string, 128);
	}
	break;
	}

	return kResultOk;
}

tresult Controller_VST3::getNoteExpressionValueByString (int32 busIndex, int16 channel, NoteExpressionTypeID id, const TChar* string, NoteExpressionValue& valueNormalized)
{
    // we accept only the first bus and 1 channel
    if (busIndex != 0 || channel != 0)
        return kResultFalse;

 	switch (id) // polyphonic pitch-bend.
	{
	case kTuningTypeID:
	{
	   // here we have to convert a given tuning string (half Tone) to a normalized value
		String wrapper ((TChar*)string);
		ParamValue tmp;
		if (wrapper.scanFloat (tmp))
		{
			valueNormalized = (tmp + 120) / 240;
			return kResultTrue;
		}
	}
	break;

	case kPanTypeID:
	{
		String wrapper ((TChar*)string);
		ParamValue tmp;
		if (wrapper.scanFloat (tmp))
		{
			valueNormalized = (tmp + 1.0) * 0.5;
			return kResultTrue;
		}
	}
	break;

	default:
	{
		String wrapper ((TChar*)string);
		ParamValue tmp;
		if (wrapper.scanFloat (tmp))
		{
			valueNormalized = tmp;
			return kResultTrue;
		}
	}
	break;
	}

	return kResultOk;
}

tresult PLUGIN_API Controller_VST3::getPhysicalUIMapping(int32 busIndex, int16 channel,
	PhysicalUIMapList& list)
{
	if (busIndex == 0 && channel == 0)
	{
		for (uint32 i = 0; i < list.count; ++i)
		{
			NoteExpressionTypeID type = kInvalidTypeID;

			switch (list.map[i].physicalUITypeID)
			{
			case kPUIXMovement:
				list.map[i].noteExpressionTypeID = kTuningTypeID;
				break;

			case kPUIYMovement:
				list.map[i].noteExpressionTypeID = kBrightnessTypeID;
				break;

			case kPUIPressure:
				list.map[i].noteExpressionTypeID = kExpressionTypeID;
				break;

			default:
				list.map[i].noteExpressionTypeID = kInvalidTypeID;
				break;
			}
		}
		return kResultTrue;
	}
	return kResultFalse;
}

}
