#pragma once

#include <atomic>
#include <map>
#include <thread>
#include <locale>
#include <codecvt>
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstphysicalui.h>
#include "Hosting/controller_holder.h"
#include "helpers/Timer.h"

namespace wrapper {
	class Controller_VST3;
}

namespace wrapper
{
struct nativeParamInfo
{
	int hostTag{ -1 }; // DAW index.
	int gmpi_id{};
};

// Manages plugin parameters: the GMPI parameter set from plugin.gmpi.xml, held in
// gmpiController's store. Knows nothing about what the plug-in is.
//
// A SynthEdit export used to also live here (this class inherited MpController and carried
// the SynthEdit engine's traffic on a private "SeBinaryMessage" channel). Since Stage 3 of
// TideSynth docs/state-management-consolidation-plan.md the export's own <Controller/>
// (SE16/se_gmpi SeGmpiController) owns that, and reaches this wrapper only through the two
// calls every wrapper makes - sePluginController->syncState() and
// gmpiController.notifyControllerOfPreset / notifyPluginController - and through ordinary
// blob parameters the exporter declares.
//-----------------------------------------------------------------------------
class Controller_VST3 :
	public gmpi::TimerClient,
	public Steinberg::Vst::EditController,
	public Steinberg::Vst::IMidiMapping,
	public Steinberg::Vst::IUnitInfo,
	public Steinberg::Vst::INoteExpressionController,
	public Steinberg::Vst::INoteExpressionPhysicalUIMapping
{

	static const int numMidiControllers = 130; // usual 128 + Bender.
	bool isInitialised;
	bool isConnected;
	std::vector<gmpi::hosting::GmpiParameter*> blobsSetBeforeConnect; // no peer yet: sent by connect()

	// MIDI CC, channel pressure and pitch bend arrive as hidden parameters the host maps MIDI onto (IMidiMapping); Processor_VST3 turns them back into MIDI.
	int supportedChannels = 0; // 16 when the plugin has a MIDI input
	std::vector<Steinberg::Vst::ParamValue> ccValues; // normalized, indexed by channel * numMidiControllers + controller
	int nativeParamCount() const { return static_cast<int>(gmpiController.nativeParams.size()); }
	int ccParamIndex(Steinberg::Vst::ParamID tag) const; // -1 when 'tag' is not a CC parameter

	// True while the host saves or restores state: anything the plugin writes then is part of that transfer, not an edit, so it must not dirty the project.
	bool hostStateTransfer_ = false;
	struct StateTransferScope
	{
		bool& flag;
		const bool was;
		explicit StateTransferScope(bool& f) : flag(f), was(f) { flag = true; }
		~StateTransferScope() { flag = was; }
	};

	std::thread::id uiThread_;
	std::atomic<bool> latencyChangePending_ = {}; // set by notify() off the UI thread, taken by onTimer().

public:
	gmpi::hosting::gmpi_controller_holder gmpiController;

	// Persistent owner of the plugin's <Controller/> subtype. The plugin uses
	// this controller to set up shared state (e.g. via host->setParameter)
	// before the editor opens. Lives as long as Controller_VST3.
	gmpi::shared_ptr<gmpi::api::IController> sePluginController;

	Controller_VST3(gmpi::hosting::pluginInfo& pinfo);
	~Controller_VST3();

	Steinberg::tresult PLUGIN_API initialize (FUnknown* context) override;
	Steinberg::tresult PLUGIN_API connect(IConnectionPoint* other) override;
	Steinberg::tresult PLUGIN_API disconnect(IConnectionPoint* other) override;
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
		// The host is saving. Give the plugin's <Controller/> its "imminent
		// save" warning (IController::syncState) so lazily-maintained state -
		// a chunk parameter refreshed on demand, like TIDE's document - is
		// current before the host serialises it.
		//
		// HONEST CAVEAT on ordering: VST3 saves read the PROCESSOR's store
		// (Processor_VST3::getState), and a refreshed blob travels there
		// asynchronously via IMessage into the ui->dsp queue, applied when
		// audio next runs. A host that reads the component's state BEFORE
		// calling here, on a suspended processor, can still capture the
		// previous refresh. The standalone has no such gap (its save is
		// synchronous, SessionState::saveNow); closing it for VST3 needs a
		// dirty-flag periodic sync, which is the VST3Adaptor's own open TODO
		// (ControllerWrapper::onTimer).
		StateTransferScope transfer(hostStateTransfer_);
		drainProcessorFeedback();
		if (sePluginController)
			sePluginController->syncState();

		return Steinberg::kResultOk;
	}

	// Everything the processor has sent the controller and nobody has read yet (its queue is
	// drained by onTimer, and on a Linux host with no editor open no timer ticks: BACKLOG E74).
	// A save must see it, or the chunk is as stale as the last tick - measured 2026-10-15: a
	// MIDI-learned parameter moved by a CC saved its OLD value until this ran. The processor
	// shadow the old wrapper kept (ProcessorStateMgrVst3) drained its FIFO at getPreset for
	// exactly this reason; this is that one behaviour, kept. Host main thread, the queue's
	// reader side, same as onTimer.
	void drainProcessorFeedback()
	{
		gmpiController.message_que_dsp_to_ui.pollMessage(&gmpiController);
	}

	// UI thread: from our own "GmpiLatency" message, or the processor's latency change.
	void notifyDawLatencyChanged();

	// TIDE BACKLOG E68 -- mint the plugin's state NOW, from the controller's
	// live store, for the paired processor's getState to write. The
	// controller is never stale (edits land in its store the moment
	// they happen), so a state minted here is complete -- knobs, patch
	// cables, document -- regardless of what the async controller->processor
	// hop has or has not delivered yet, and regardless of which side's
	// getState the host happens to call first. Runs on the host's own
	// calling thread; per the VST3 spec state calls arrive on the main
	// thread, which is also where every editor operation already runs.
	std::string mintFreshPresetXml()
	{
		// The plugin's <Controller/> refreshes lazily-maintained state (TIDE's
		// document chunk, a SynthEdit export's engine preset) into the store
		// first -- synchronous: syncState -> host->setParameter -> the store,
		// all in this process on this thread.
		StateTransferScope transfer(hostStateTransfer_);
		drainProcessorFeedback();
		if (sePluginController)
			sePluginController->syncState();

		return gmpiController.getPresetXml();
	}
	bool onTimer() override;

	virtual Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value ) override;

	// IDependent
	virtual void PLUGIN_API update( FUnknown* changedUnknown, Steinberg::int32 message ) override;

	// MIDI Mapping.
	Steinberg::tresult PLUGIN_API getMidiControllerAssignment (Steinberg::int32 busIndex, Steinberg::int16 channel, Steinberg::Vst::CtrlNumber midiControllerNumber, Steinberg::Vst::ParamID& tag/*out*/) override;

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
		if (unitIndex != 0)
			return Steinberg::kResultFalse;

		info.id = Steinberg::Vst::kRootUnitId;
		info.name[0] = 0;
		info.parentUnitId = Steinberg::Vst::kNoParentUnitId;
		info.programListId = Steinberg::Vst::kNoProgramListId; // GMPI has no program lists; presets travel as state.
		return Steinberg::kResultOk;
	}

	/** Component intern program structure. */
	/** Gets the count of Program List. */
	Steinberg::int32 PLUGIN_API getProgramListCount() override { return 0; }

	/** Gets for a given index the Program List Info. */
	Steinberg::tresult PLUGIN_API getProgramListInfo(Steinberg::int32 listIndex, Steinberg::Vst::ProgramListInfo& info /*out*/) override
	{
		return Steinberg::kResultFalse;
	}

	/** Gets for a given program list ID and program index its program name. */
	Steinberg::tresult PLUGIN_API getProgramName(Steinberg::Vst::ProgramListID listId, Steinberg::int32 programIndex, Steinberg::Vst::String128 name /*out*/) override
	{
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
		return static_cast<Steinberg::int32>(nativeParamCount() + ccValues.size());
	}
	Steinberg::tresult PLUGIN_API getParameterInfo(Steinberg::int32 paramIndex, Steinberg::Vst::ParameterInfo& info) override;
	Steinberg::tresult PLUGIN_API getParamStringByValue(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string) override;
	Steinberg::tresult PLUGIN_API getParamValueByString(Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized) override;
	Steinberg::Vst::ParamValue PLUGIN_API normalizedParamToPlain(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized) override
	{
		if (ccParamIndex(tag) >= 0)
			return valueNormalized;

		if (tag < 0 || tag >= static_cast<int>(gmpiController.nativeParams.size()))
			return 0.0;

		const auto& p = *gmpiController.nativeParams[tag];

		return p.normalized2Real(valueNormalized);
	}
	Steinberg::Vst::ParamValue PLUGIN_API plainParamToNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue plainValue) override
	{
		if (ccParamIndex(tag) >= 0)
			return plainValue;

		if (tag < 0 || tag >= static_cast<int>(gmpiController.nativeParams.size()))
			return 0.0;

		const auto& p = *gmpiController.nativeParams[tag];

		return p.real2Normalized(plainValue);
	}
	Steinberg::Vst::ParamValue PLUGIN_API getParamNormalized(Steinberg::Vst::ParamID tag) override
	{
		if (const auto i = ccParamIndex(tag); i >= 0)
			return ccValues[i];

		if (tag < 0 || tag >= static_cast<int>(gmpiController.nativeParams.size()))
			return 0.0; // a ParamValue, not a tresult

		const auto& p = *gmpiController.nativeParams[tag];

		return p.normalisedValue();
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
	Steinberg::tresult PLUGIN_API setUnitProgramData(Steinberg::int32 listOrUnitId, Steinberg::int32 programIndex, Steinberg::IBStream* data) override { return Steinberg::kResultFalse; }

	bool sendMessageToProcessor(const void* data, int size, const char* messageId = "BinaryMessage");
};

}
