#ifndef __adelaycontroller__
#define __adelaycontroller__

#include <locale>
#include <codecvt>
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstphysicalui.h>
#include <map>
#include "wrapper/common/StagingMemoryBuffer.h"
#include "wrapper/common/interThreadQue.h"
#include "wrapper/common/Controller.h"
#include "wrapper/common/MpParameter.h"
#include "wrapper/common/conversion.h"

struct pluginInfoSem;

namespace wrapper {
	class VST3Controller;
}

namespace wrapper
{ 
class MpParameterVst3 : public MpParameter_native
{
	wrapper::VST3Controller* vst3Controller = {};
	int hostTag = -1;	// index as set in SE, not nesc sequential.

public:
	MpParameterVst3(wrapper::VST3Controller* controller, int ParameterTag, bool isInverted);
	
	int getNativeTag() override { return hostTag; }

	// some hosts can't handle parameter min > max, if so calculate normalize in reverse.
	float convertNormalized(float normalised) const
	{
		return isInverted_ ? 1.0f - normalised : normalised;
	}

	void updateProcessor(gmpi::Field fieldId, int32_t voice) override;

	// not required for VST3.
	void upDateImmediateValue() override {}
	void updateDawUnsafe(const std::string& rawValue) override {}

	bool isInverted_ = false;
};

// Manages plugin parameters.
//-----------------------------------------------------------------------------
class VST3Controller :
	public MpController,
	public Steinberg::Vst::EditController,
	public Steinberg::Vst::IMidiMapping,
	public Steinberg::Vst::IUnitInfo,
	public Steinberg::Vst::INoteExpressionController,
	public Steinberg::Vst::INoteExpressionPhysicalUIMapping
{
	static const int numMidiControllers = 130; // usual 128 + Bender.
	bool isInitialised;
	bool isConnected;
//	std::wstring_convert<std::codecvt_utf8<wchar_t>> convert;
	std::map<int, MpParameterVst3* > tagToParameter;	// DAW parameter Index to parameter
	std::vector<MpParameterVst3* > vst3Parameters;      // flat list.

	// Hold data until timer can put it in VST3 queue mechanism.
	StagingMemoryBuffer queueToDsp_;
	int supportedChannels = 1;

public:
	VST3Controller(pluginInfoSem& pinfo);
	~VST3Controller();

	Steinberg::tresult PLUGIN_API initialize (FUnknown* context) override;
	Steinberg::tresult PLUGIN_API connect(IConnectionPoint* other) override;
	Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message ) override;
	virtual Steinberg::IPlugView* PLUGIN_API createView (Steinberg::FIDString name) override;

	virtual Steinberg::tresult PLUGIN_API setComponentState (Steinberg::IBStream* state) override;

	//testing.
	Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override
	{
		return Steinberg::kResultOk;
	}
	Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override
	{
		return Steinberg::kResultOk;
	}
	void ParamGrabbed(MpParameter_native* param) override;
	void ParamToProcessorViaHost(MpParameterVst3* param, int32_t voice = 0);

	MpParameterVst3* getDawParameter(int nativeTag)
	{
		auto it = tagToParameter.find(nativeTag);
		if (it != tagToParameter.end())
		{
			return (*it).second;
		}

		return {};
	}

	virtual Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value ) override;

	// IDependent
	virtual void PLUGIN_API update( FUnknown* changedUnknown, Steinberg::int32 message ) override;

	// MIDI Mapping.
	Steinberg::tresult PLUGIN_API getMidiControllerAssignment (Steinberg::int32 busIndex, Steinberg::int16 channel, Steinberg::Vst::CtrlNumber midiControllerNumber, Steinberg::Vst::ParamID& tag/*out*/) override;

//	static FUnknown* createInstance (void*) { return (IEditController*)new VST3Controller (); }

	//-----------------------------
	DELEGATE_REFCOUNT (EditController)
	Steinberg::tresult PLUGIN_API queryInterface (const char* iid, void** obj) override;
	//-----------------------------

    //---from INoteExpressionController
	Steinberg::int32 PLUGIN_API getNoteExpressionCount (Steinberg::int32 busIndex, Steinberg::int16 channel) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getNoteExpressionInfo (Steinberg::int32 busIndex, Steinberg::int16 channel, Steinberg::int32 noteExpressionIndex, Steinberg::Vst::NoteExpressionTypeInfo& info) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getNoteExpressionStringByValue (Steinberg::int32 busIndex, Steinberg::int16 channel, Steinberg::Vst::NoteExpressionTypeID id, Steinberg::Vst::NoteExpressionValue valueNormalized , Steinberg::Vst::String128 string) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getNoteExpressionValueByString (Steinberg::int32 busIndex, Steinberg::int16 channel, Steinberg::Vst::NoteExpressionTypeID id, const Steinberg::Vst::TChar* string, Steinberg::Vst::NoteExpressionValue& valueNormalized) SMTG_OVERRIDE;

	/** Fills the list of mapped [physical UI (in) - note expression (out)] for a given bus index
 * and channel. */
	Steinberg::tresult PLUGIN_API getPhysicalUIMapping(Steinberg::int32 busIndex, Steinberg::int16 channel,
		Steinberg::Vst::PhysicalUIMapList& list) override;


	// IUnitInfo
	//------------------------------------------------------------------------
	/** Returns the flat count of units. */
	Steinberg::int32 PLUGIN_API getUnitCount() override { return 1; }

	/** Gets UnitInfo for a given index in the flat list of unit. */
	Steinberg::tresult PLUGIN_API getUnitInfo(Steinberg::int32 unitIndex, Steinberg::Vst::UnitInfo& info /*out*/) override
	{
		info.id = Steinberg::Vst::kRootUnitId;
		info.name[0] = 0;
		info.parentUnitId = Steinberg::Vst::kNoParentUnitId;
		info.programListId = 0;
		return Steinberg::kResultOk;
	}

	/** Component intern program structure. */
	/** Gets the count of Program List. */
	Steinberg::int32 PLUGIN_API getProgramListCount() override { return 1; } // number of program lists. Always 1.

	/** Gets for a given index the Program List Info. */
	Steinberg::tresult PLUGIN_API getProgramListInfo(Steinberg::int32 listIndex, Steinberg::Vst::ProgramListInfo& info /*out*/) override
	{
		if (listIndex == 0)
		{
			info.id = Steinberg::Vst::kRootUnitId;
			info.name[0] = 0;
//			info.programCount = (Steinberg::int32) factoryPresetNames.size();
			info.programCount = (Steinberg::int32) presets.size();
			return Steinberg::kResultOk;
		}
		return Steinberg::kResultFalse;
	}

	/** Gets for a given program list ID and program index its program name. */
	Steinberg::tresult PLUGIN_API getProgramName(Steinberg::Vst::ProgramListID listId, Steinberg::int32 programIndex, Steinberg::Vst::String128 name /*out*/) override
	{
		const int kVstMaxProgNameLen = 24;
//		if (programIndex < (int32)factoryPresetNames.size())
		if (programIndex < (Steinberg::int32)presets.size())
		{
//			for (int i = 0; i < kVstMaxProgNameLen && i <= (int)factoryPresetNames[programIndex].size(); ++i)
			for (int i = 0; i < kVstMaxProgNameLen && i <= (int)presets[programIndex].name.size(); ++i)
			{
//				name[i] = factoryPresetNames[programIndex][i];
				name[i] = presets[programIndex].name[i];
			}
			return Steinberg::kResultOk;
		}
		return Steinberg::kResultFalse;
	}

	/** Gets for a given program list ID, program index and attributeId the associated attribute value. */
	Steinberg::tresult PLUGIN_API getProgramInfo(Steinberg::Vst::ProgramListID listId, Steinberg::int32 programIndex,
		Steinberg::Vst::CString attributeId /*in*/, Steinberg::Vst::String128 attributeValue /*out*/) override
	{
		return Steinberg::kResultFalse; // no idea what this is for.

		//attributeValue[0] = 0;
		//return kResultOk;
	}

	/** Returns kResultTrue if the given program index of a given program list ID supports PitchNames. */
	virtual Steinberg::tresult PLUGIN_API hasProgramPitchNames(Steinberg::Vst::ProgramListID listId, Steinberg::int32 programIndex) override { return Steinberg::kResultFalse; }

	/** Gets the PitchName for a given program list ID, program index and pitch.
	If PitchNames are changed the Plug-in should inform the host with IUnitHandler::notifyProgramListChange. */
	virtual Steinberg::tresult PLUGIN_API getProgramPitchName(Steinberg::Vst::ProgramListID listId, Steinberg::int32 programIndex,
		Steinberg::int16 midiPitch, Steinberg::Vst::String128 name /*out*/) override {
		return Steinberg::kResultFalse;
	}

	// Parameter overrides.
	Steinberg::int32 PLUGIN_API getParameterCount() override
	{
		return static_cast<int>(vst3Parameters.size());
	}
	Steinberg::tresult PLUGIN_API getParameterInfo(Steinberg::int32 paramIndex, Steinberg::Vst::ParameterInfo& info) override;
	Steinberg::tresult PLUGIN_API getParamStringByValue(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string) override;
	Steinberg::tresult PLUGIN_API getParamValueByString(Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized) override
	{
		if (auto p = getDawParameter(tag); p)
		{
			valueNormalized = p->convertNormalized(p->stringToNormalised(ToWstring(string)));
			return Steinberg::kResultOk;
		}

		return Steinberg::kInvalidArgument;
	}
	Steinberg::Vst::ParamValue PLUGIN_API normalizedParamToPlain(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized) override
	{
		if (auto p = getDawParameter(tag); p)
		{
			return p->normalisedToReal(p->convertNormalized(valueNormalized));
		}

		return 0.0;
	}
	Steinberg::Vst::ParamValue PLUGIN_API plainParamToNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue plainValue) override
	{
		if (auto p = getDawParameter(tag); p)
		{
			return p->convertNormalized(p->RealToNormalized(plainValue));
		}

		return 0.0;
	}
	Steinberg::Vst::ParamValue PLUGIN_API getParamNormalized(Steinberg::Vst::ParamID tag) override
	{
		if (auto p = getDawParameter(tag); p)
		{
//            _RPT2(_CRT_WARN, "getParamNormalized() => DAW %d %f\n", tag, p->getNormalized());
      
			return p->convertNormalized(p->getNormalized());
		}

		return 0.0;
	}

	// units selection --------------------
	/** Gets the current selected unit. */
	Steinberg::Vst::UnitID PLUGIN_API getSelectedUnit() override { return 0; }

	/** Sets a new selected unit. */
	Steinberg::tresult PLUGIN_API selectUnit(Steinberg::Vst::UnitID unitId) override { return Steinberg::kResultOk; }

	/** Gets the according unit if there is an unambiguous relation between a channel or a bus and a unit.
	This method mainly is intended to find out which unit is related to a given MIDI input channel. */
	Steinberg::tresult PLUGIN_API getUnitByBus(Steinberg::Vst::MediaType type, Steinberg::Vst::BusDirection dir, Steinberg::int32 busIndex,
		Steinberg::int32 channel, Steinberg::Vst::UnitID& unitId /*out*/) override
	{
		unitId = 0;
		return Steinberg::kResultFalse;
	}

	/** Receives a preset data stream.
	- If the component supports program list data (IProgramListData), the destination of the data
	stream is the program specified by list-Id and program index (first and second parameter)
	- If the component supports unit data (IUnitData), the destination is the unit specified by the first
	parameter - in this case parameter programIndex is < 0). */
	Steinberg::tresult PLUGIN_API setUnitProgramData(Steinberg::int32 listOrUnitId, Steinberg::int32 programIndex, Steinberg::IBStream* data) override { return Steinberg::kResultOk; }

	void ResetProcessor() override;

	// Presets
	void setPresetXmlFromSelf(const std::string& xml) override;
	void setPresetFromSelf(DawPreset const* preset) override;

	std::wstring getNativePresetExtension() override
	{
		return L"vstpreset";
	}
	std::vector< MpController::presetInfo > scanFactoryPresets() override;
	platform_string calcFactoryPresetFolder();
	std::string getFactoryPresetXml(std::string filename) override;

	void loadFactoryPreset(int index, bool fromDaw) override;
	void saveNativePreset(const char* filename, const std::string& presetName, const std::string& xml) override;
	std::string loadNativePreset(std::wstring sourceFilename) override;

	void OnLatencyChanged() override;
	bool sendMessageToProcessor(const void* data, int size);

	MpParameter* nativeGetParameterByIndex(int nativeIndex)
	{
		assert(isInitialized);

		if (nativeIndex >= 0 && nativeIndex < static_cast<int>(vst3Parameters.size()))
			return vst3Parameters[nativeIndex];

		return nullptr;
	}

	MpParameter_native* makeNativeParameter(int ParameterTag, bool isInverted) override
	{
		auto param = new wrapper::MpParameterVst3(
			this,
			ParameterTag,
			isInverted
		);

		tagToParameter.insert({ ParameterTag, param });
		vst3Parameters.push_back(param);

		return param;
	}

	bool onTimer() override;

	IWriteableQue* getQueueToDsp() override
	{
		return &queueToDsp_;
	}

	void setPinFromUi(int32_t pinId, int32_t voice, int32_t size, const void* data);
	void initUi(gmpi::api::IParameterObserver* gui);
};

}
#endif
