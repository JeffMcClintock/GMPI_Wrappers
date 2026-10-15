#include <algorithm>
#include "base/source/fstring.h"
#include "base/source/updatehandler.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/vst/vstpresetfile.h"
//#include "public.sdk/source/common/memorystream.h"
#include "Controller_VST3.h"
//#include "MyVstPluginFactory.h"
#include "Processor_VST3.h"
#include "StateStreamRead.h"
#include "GmpiApiEditor.h"
//#include "wrapper/common/se_datatypes.h" // kill this
#include "wrapper/common/unicode_conversion.h"
#include "Hosting/gmpi_factory.h"
#include "BundleInfo.h"


#ifdef _WIN32
#include "SEVSTGUIEditorWin.h"
#elif defined(__APPLE__)
#include "SEVSTGUIEditorMac.h"
#else
#include "SEVSTGUIEditorLinux.h"
#if GMPI_VST3_WAYLAND
#include "SEVSTGUIEditorWayland.h"
#endif
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

typedef gmpi::ReturnCode(*MP_DllEntry)(void**);

namespace wrapper
{

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

	// A GUI drag is one host gesture: begin on grab, end on release, performEdits in between.
	gmpiController.notifyDawGrab = [this](gmpi::hosting::GmpiParameter const* param, bool grabbed)
		{
			if (grabbed)
				beginEdit(param->info->dawTag);
			else
				endEdit(param->info->dawTag);
		};

	gmpiController.notifyDawStateChanged = [this]()
		{
			if (!hostStateTransfer_)
				setDirty(true); // no-op if the host lacks IComponentHandler2
		};

	// Blob parameters can't ride performEdit (doubles only), so the holder
	// hands them to us already framed for the processor's ui->dsp queue
	// ("ppc3"; see GmpiParameter::getQueMessage). We only move the bytes
	// across - Processor_VST3::notify pushes them into that queue verbatim.
	gmpiController.sendNonNativeParameterToProcessor = [this](gmpi::hosting::GmpiParameter* param)
		{
			// initialize() runs before connect(), and a message with no peer is dropped. A stateful
			// blob reaches the processor through its own setState, which a replay could overwrite.
			if (!isConnected)
			{
				if (!param->info->is_stateful && std::find(blobsSetBeforeConnect.begin(), blobsSetBeforeConnect.end(), param) == blobsSetBeforeConnect.end())
					blobsSetBeforeConnect.push_back(param);
				return;
			}

			struct MemStream : gmpi::hosting::my_output_stream
			{
				std::vector<uint8_t> bytes;
				void Write(const void* buf, unsigned int byteCount) override
				{
					const auto* p = static_cast<const uint8_t*>(buf);
					bytes.insert(bytes.end(), p, p + byteCount);
				}
			} frame;

			const auto messageLength = param->queryQueMessageLength(0);
			param->getQueMessage(frame, messageLength);

			sendMessageToProcessor(frame.bytes.data(), static_cast<int>(frame.bytes.size()));
		};

	gmpiController.init(pinfo);
}

Controller_VST3::~Controller_VST3()
{
	stopTimer();
}

tresult PLUGIN_API Controller_VST3::connect(IConnectionPoint* other)
{
	auto r = EditController::connect(other);

	isConnected = true;

	// TIDE BACKLOG E68 -- hand the processor OUR pointer, once, so its
	// getState can pull fresh state synchronously instead of serialising
	// whatever the async message hop last delivered. A MESSAGE rather than a
	// cast of `other`: hosts may interpose a connection proxy, and a proxy
	// forwards messages faithfully while defeating any dynamic_cast. The
	// pointer is only meaningful in-process -- which this wrapper already
	// assumes (parameter 0 publishes the app object's address the same way).
	if (auto* msg = allocateMessage(); msg)
	{
		msg->setMessageID("GmpiCtlPtr");
		msg->getAttributes()->setInt("ptr", (Steinberg::int64)(intptr_t)this);
		sendMessage(msg);
		msg->release();
	}

	// Their current values, in the order they were first set.
	for (auto* param : blobsSetBeforeConnect)
		gmpiController.sendNonNativeParameterToProcessor(param);
	blobsSetBeforeConnect.clear();

#if 0

	// Can only init controllers after both VST controller initialised AND controller is connected to processor.
	// So VST2 wrapper aeffect pointer makes it to Processor.
	if(isConnected && isInitialised)
		initSemControllers();
#endif

	return r;
}

// Blobs set while there is no peer wait for the next connect() again.
tresult PLUGIN_API Controller_VST3::disconnect(IConnectionPoint* other)
{
	isConnected = false;
	return EditController::disconnect(other);
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

	// The plugin's latency changed (Processor_VST3::process). A host may deliver this on the
	// processor's thread, and restartComponent is UI-thread only, so defer it to onTimer.
	if (!strcmp(message->getMessageID(), "GmpiLatency"))
	{
		if (std::this_thread::get_id() == uiThread_)
			notifyDawLatencyChanged();
		else
			latencyChangePending_.store(true, std::memory_order_release);
		return kResultOk;
	}

	return EditController::notify( message );
}

bool Controller_VST3::sendMessageToProcessor(const void* data, int size, const char* messageId)
{
	auto message = allocateMessage();
	if (message)
	{
		FReleaser msgReleaser(message);
		message->setMessageID(messageId);

		message->getAttributes()->setBinary("MyData", data, size);
		sendMessage(message);
	}

	return true;
}
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

//-----------------------------------------------------------------------------
tresult PLUGIN_API Controller_VST3::initialize (FUnknown* context)
{
	UpdateHandler::instance();
	uiThread_ = std::this_thread::get_id();

	tresult result = EditController::initialize (context);
	if (result == kResultTrue)
	{
		// Modules database, needed here to load any controllers. e.g. VST2 child plugins.
		{
//?			ModuleFactory()->RegisterExternalPluginsXmlOnce(nullptr);
		}

		// CC tags start at MidiControllersParameterId, so the native tags (their indexes) must stay below it.
		const bool hasMidiIn = gmpiController.info && countPins(*gmpiController.info, gmpi::PinDirection::In, gmpi::PinDatatype::Midi) > 0;
		const bool tagsFit = nativeParamCount() <= MidiControllersParameterId;
		assert(tagsFit);

		supportedChannels = hasMidiIn && tagsFit ? 16 : 0;
		ccValues.assign(supportedChannels * numMidiControllers, 0.0);
		for (int chan = 0; chan < supportedChannels; ++chan)
			ccValues[chan * numMidiControllers + kPitchBend] = 0.5; // centred
	}

	isInitialised = true;

	// Instantiate the plugin's <Controller/> subtype (if it has one). This is
	// where plugins can stash persistent state — e.g. construct an app object
	// and call host->setParameter to publish a pointer to it via a Blob param,
	// so subsequent editor instances can find it via initUi.
	{
		auto info = gmpi::hosting::factory::getInstance().getPluginInfo();
		if (info)
		{
			auto controllerUnknown = gmpi::hosting::factory::getInstance().createInstance(info->id.c_str(), gmpi::api::PluginSubtype::Controller);
			if (controllerUnknown)
			{
				sePluginController = controllerUnknown.as<gmpi::api::IController>();
				if (sePluginController)
				{
					sePluginController->initialize(static_cast<gmpi::api::IControllerHost*>(&gmpiController), 0);

					// Its IParameterObserver side, for the holder to tell of processor- and
					// DAW-originated changes (a SynthEdit export's panel follows the DAW this way).
					gmpiController.pluginController = sePluginController.as<gmpi::api::IParameterObserver>().get();
				}
			}
		}
	}

#if 0

	// Can only init controllers after both VST controller initialised AND controller is connected to processor.
	// So VST2 wrapper aeffect pointer makes it to Processor.
	if (isConnected && isInitialised)
		initSemControllers();
#endif

	const int timerPeriodMs = 15; // ~60Hz, same as the drawing frame's tick. (16ms tends to miss v-sync)
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
	if (busIndex != 0 || channel < 0 || channel >= supportedChannels || midiControllerNumber < 0 || midiControllerNumber >= numMidiControllers)
		return kResultFalse;

	tag = MidiControllersParameterId + channel * numMidiControllers + midiControllerNumber;
	return kResultTrue;
}

int Controller_VST3::ccParamIndex(ParamID tag) const
{
	const auto i = static_cast<int64_t>(tag) - MidiControllersParameterId;
	return i >= 0 && i < static_cast<int64_t>(ccValues.size()) ? static_cast<int>(i) : -1;
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

	// The size is the editor's to say: each platform editor's constructor asks it through
	// VST3EditorBase::measurePreferredSize(), and these two are only the fallback for an
	// editor that expresses no preference. This used to read a SynthEdit export's
	// gui.se.json here instead, because its panel view never answered measure(); it does
	// now (SeGmpiEditor::measure, found when the CLAP wrapper - which only ever asked -
	// opened it at 1x1), so this wrapper no longer needs to know what a SynthEdit bundle
	// looks like. Measured after the change: the Linux editor still opens at 480x480.
	int width { 200 };
	int height{ 200 };
#ifdef _WIN32
	return new SEVSTGUIEditorWin(*info, editor, this, width, height);
#elif defined(__APPLE__)
	return new SEVSTGUIEditorMac(*info, editor, this, width, height);
#else
#if GMPI_VST3_WAYLAND
	// Wayland first when the host offers it (VST3 3.8.0+), X11 otherwise. The
	// probe is for IWaylandHost, not for WAYLAND_DISPLAY: a host running under
	// Wayland may still embed its plugins through XWayland, and only the host
	// knows which it does.
	if (SEVSTGUIEditorWayland::hostSupportsWayland())
		return new SEVSTGUIEditorWayland(*info, editor, this, width, height);
#endif

	return new SEVSTGUIEditorLinux(*info, editor, this, width, height);
#endif
// todo init all params and pins			initializeGui(&helper)
}

// Preset Loaded.
tresult PLUGIN_API Controller_VST3::setComponentState (IBStream* state)
{
	std::string chunk;
	if (!gmpi_vst3::readSizedChunk(state, chunk))
		return kResultFalse; // short or corrupt stream: keep the current state.

	// Fail safe, for the same reason as Processor_VST3::setState - this is the
	// controller half of the same main-thread project-load path, so an escaping
	// exception aborts the DAW just as surely.
	try
	{
		StateTransferScope transfer(hostStateTransfer_);
		gmpiController.setPresetXmlFromDaw(chunk);

		// Hand the restored parameters to the plug-in's own <Controller/>.
		// It registers as an IParameterObserver, but nothing ever called it -
		// sePluginController was only ever initialize()d - so a plug-in whose
		// state IS a parameter had no way to learn its state had been restored,
		// and started blank however good the preset was.
		//
		// The loop that does it moved into the holder, because restoring state
		// without telling the plug-in's controller is a mistake every wrapper
		// can make and the standalone was about to make next. Null is fine: a
		// plug-in that declares no <Controller/> leaves this empty.
		gmpiController.notifyControllerOfPreset(sePluginController.get());
	}
	catch (...)
	{
		return kResultFalse;
	}

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

	if (const auto i = ccParamIndex(tag); i >= 0)
	{
		ccValues[i] = value;
		return kResultTrue;
	}

	if (tag >= static_cast<ParamID>(nativeParamCount()))
		return kInvalidArgument;

	if (auto p = gmpiController.patchManager.setParameterNormalised(gmpiController.nativeParams[tag]->info->id, value); p) // todo avoid lookup when we already have pointer to parameter
	{
		gmpiController.notifyGui(p);
		gmpiController.notifyPluginController(p); // the plug-in's own controller follows the DAW too
	}

	return kResultTrue;
}
// UI thread: from our own "GmpiLatency" message.
void Controller_VST3::notifyDawLatencyChanged()
{
	if (auto* handler = getComponentHandler(); handler) // null until the host connects it
		handler->restartComponent(kLatencyChanged);
}

namespace
{
// CC and channel pressure display as 0..127, pitch bend as -8192..8191.
double ccToDisplay(int controller, ParamValue normalized)
{
	return controller == kPitchBend ? std::round(normalized * 16383.0) - 8192.0 : std::round(normalized * 127.0);
}
ParamValue ccFromDisplay(int controller, double display)
{
	const auto normalized = controller == kPitchBend ? (display + 8192.0) / 16383.0 : display / 127.0;
	return std::clamp(normalized, 0.0, 1.0);
}

bool isEnum(const gmpi::hosting::paramInfo& info)
{
	return (info.datatype == gmpi::PinDatatype::Int32 || info.datatype == gmpi::PinDatatype::Int64) && !info.enum_entries.empty();
}

// A plain value as the host should show it. Enums by name (a GMPI enum's value is its entry's position); floats as SynthEdit's
// sliders show them (SliderFloatToString): 3 decimals, more for small values, never "-0.000".
std::string realToDisplay(const gmpi::hosting::paramInfo& info, double real)
{
	if (isEnum(info))
		return info.enum_entries[std::clamp(static_cast<int>(std::round(real)), 0, static_cast<int>(info.enum_entries.size()) - 1)].name;

	switch (info.datatype)
	{
	case gmpi::PinDatatype::Bool:
		return real >= 0.5 ? "On" : "Off";
	case gmpi::PinDatatype::Int32:
	case gmpi::PinDatatype::Int64:
		return std::to_string(std::llround(real));
	default:
		break;
	}

	int precision = 3;
	if (auto magnitude = std::fabs(real); magnitude > 1e-8)
	{
		for (magnitude *= 100.0; precision < 8 && magnitude < 1.0; magnitude *= 10.0)
			++precision;
	}

	char text[64]{};
	snprintf(text, sizeof(text), "%.*f", precision, real);
	std::string result = text;
	if (result[0] == '-' && result.find_first_not_of("-0.") == std::string::npos)
		result.erase(0, 1);
	return result;
}

// Text typed into the host, back to a plain value: an enum name or On/Off (any case), else a number. False if it is neither.
bool displayToReal(const gmpi::hosting::paramInfo& info, const std::string& text, double& real)
{
	auto lower = [](std::string t) { for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
	const auto typed = lower(text);

	if (isEnum(info))
	{
		for (size_t i = 0; i < info.enum_entries.size(); ++i)
		{
			if (lower(info.enum_entries[i].name) == typed)
			{
				real = static_cast<double>(i);
				return true;
			}
		}
	}
	else if (info.datatype == gmpi::PinDatatype::Bool)
	{
		if (typed == "on" || typed == "true")  { real = 1.0; return true; }
		if (typed == "off" || typed == "false") { real = 0.0; return true; }
	}

	char* end{};
	real = std::strtod(text.c_str(), &end);
	return end != text.c_str();
}
}

Steinberg::tresult PLUGIN_API Controller_VST3::getParamValueByString(Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized)
{
	const auto cc = ccParamIndex(tag);
	if (cc < 0 && (tag < 0 || tag >= static_cast<int>(gmpiController.nativeParams.size())))
		return Steinberg::kInvalidArgument;

	auto valueString = wrapper::JmUnicodeConversions::ToWstring(string);

	wchar_t* endPtr{};
	const auto real = wcstof(valueString.c_str(), &endPtr);

	if (cc >= 0)
	{
		valueNormalized = ccFromDisplay(cc % numMidiControllers, real);
		return Steinberg::kResultOk;
	}

	const auto& p = *gmpiController.nativeParams[tag];
	double typed{};
	if (!displayToReal(*p.info, wrapper::JmUnicodeConversions::WStringToUtf8(valueString), typed))
		return Steinberg::kResultFalse;

	valueNormalized = std::clamp(p.real2Normalized(typed), 0.0, 1.0);
	return Steinberg::kResultOk;
}

tresult Controller_VST3::getParameterInfo(int32 paramIndex, ParameterInfo& returnInfo)
{
	if (const auto i = paramIndex - nativeParamCount(); i >= 0 && i < static_cast<int>(ccValues.size()))
	{
		const int channel = i / numMidiControllers;
		const int controller = i % numMidiControllers;

		std::string name = controller == kAfterTouch ? "Channel Pressure" : controller == kPitchBend ? "Pitch Bend" : "MIDI CC " + std::to_string(controller);
		name += " [" + std::to_string(channel + 1) + "]";
		const auto name16 = JmUnicodeConversions::ToUtf16(name);

		returnInfo = {};
		returnInfo.id = MidiControllersParameterId + i;
		_tstrncpy(returnInfo.title, (const TChar*)name16.c_str(), static_cast<Steinberg::uint32>(std::size(returnInfo.title)));
		_tstrncpy(returnInfo.shortTitle, (const TChar*)name16.c_str(), static_cast<Steinberg::uint32>(std::size(returnInfo.shortTitle)));
		returnInfo.defaultNormalizedValue = controller == kPitchBend ? 0.5 : 0.0;
		returnInfo.unitId = kRootUnitId;
		returnInfo.flags = ParameterInfo::kIsHidden; // MIDI-mapping only: not automatable, not listed in the host's generic editor
		return kResultOk;
	}

	if( paramIndex < 0 || paramIndex >= static_cast<int>(gmpiController.nativeParams.size()))
		return kInvalidArgument;

	const auto& p = *gmpiController.nativeParams[paramIndex];

	returnInfo.flags = Steinberg::Vst::ParameterInfo::kCanAutomate;
	returnInfo.defaultNormalizedValue = std::clamp(p.real2Normalized(atof(p.info->default_value_s.c_str())), 0.0, 1.0); // as GmpiParameter::setToDefault reads it

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
	if (const auto i = ccParamIndex(tag); i >= 0)
	{
		const auto s = JmUnicodeConversions::ToUtf16(std::to_string(static_cast<int>(ccToDisplay(i % numMidiControllers, valueNormalized))));
		strncpy16(string, (const TChar*)s.c_str(), 128);
		return kResultOk;
	}

	if (tag < 0 || tag >= static_cast<int>(gmpiController.nativeParams.size()))
		return kInvalidArgument;

	const auto& p = *gmpiController.nativeParams[tag];
	const auto s = JmUnicodeConversions::ToUtf16(realToDisplay(*p.info, p.normalized2Real(valueNormalized)));
	strncpy16(string, (const TChar*)s.c_str(), 128);
	return kResultOk;
}

bool Controller_VST3::onTimer()
{
	if (latencyChangePending_.exchange(false, std::memory_order_acq_rel))
		notifyDawLatencyChanged();

	// parameter updates from the Processor
	gmpiController.message_que_dsp_to_ui.pollMessage(&gmpiController);

	return true;
}

namespace
{
// The note expressions offered to the host; Processor_VST3 handles each one. getPhysicalUIMapping routes MPE gestures to them.
constexpr NoteExpressionTypeID noteExpressionTypes[] = { kTuningTypeID, kExpressionTypeID, kBrightnessTypeID };
}

int32 Controller_VST3::getNoteExpressionCount(int32 busIndex, int16 channel)
{
    // we accept only the first bus and 1 channel
    if (busIndex != 0 || channel != 0)
        return 0;

	return static_cast<int32>(std::size(noteExpressionTypes));
}

tresult Controller_VST3::getNoteExpressionInfo (int32 busIndex, int16 channel, int32 noteExpressionIndex, NoteExpressionTypeInfo& info)
{
    // we accept only the first bus and 1 channel
    if (busIndex != 0 || channel != 0 || noteExpressionIndex < 0 || noteExpressionIndex >= static_cast<int32>(std::size(noteExpressionTypes)))
        return kResultFalse;

	memset (&info, 0, sizeof (NoteExpressionTypeInfo));

	info.typeId = noteExpressionTypes[noteExpressionIndex];
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
