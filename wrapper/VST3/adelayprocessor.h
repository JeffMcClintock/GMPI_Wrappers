#ifndef __adelayprocessor__
#define __adelayprocessor__

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "GmpiApiAudio.h"
#include "GmpiSdkCommon.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <array>
#include <condition_variable>
#include <optional>
#include "GmpiMidi.h"
#include "wrapper/common/dynamic_linking.h"
#include "Hosting/xml_spec_reader.h"
#include "Hosting/processor_holder.h"
#include "Hosting/message_queues.h"

static const int MidiControllersParameterId = 10000;

enum class VoiceAllocationHint {Keyboard, MPE};

namespace wrapper
{

// to work around Steinberg Interfaces having incompatible addRef etc
class GmpiBaseClass :public gmpi::api::IProcessorHost
{
public:
	GMPI_REFCOUNT_NO_DELETE;
};



//-----------------------------------------------------------------------------
typedef int64_t timestamp_t;

class SeProcessor : public Steinberg::Vst::AudioEffect, public GmpiBaseClass //, public IShellServices, public IProcessorMessageQues
{
public:
	SeProcessor (gmpi::hosting::pluginInfo& pinfo);
	~SeProcessor ();
	
	Steinberg::tresult PLUGIN_API initialize (FUnknown* context) override;
	Steinberg::uint32 PLUGIN_API getLatencySamples() override;
	Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns, Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;

	Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) override;
	Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) override;

	Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) override;
	Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) override;

	Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override;

//	static FUnknown* createInstance (void*) { return (IAudioProcessor*)new SeProcessor (); }

	void reInitialise();
#if 0 // TODO
	// IShellServices
	void flushPendingParameterUpdates() override {}
#endif
	void onQueDataAvailable();// override;

	// IProcessorMessageQues
	gmpi::hosting::IWriteableQue* MessageQueToGui() // override
	{
		return &m_message_que_dsp_to_ui;
	}
#if 0 // TODO
	interThreadQue* ControllerToProcessorQue() override
	{
		return &m_message_que_ui_to_dsp;
	}
	void Service() //override
	{
		// If any data waiting, either from ServiceWaiter or any other queue user, send it via VST3 binary message.
		if (m_message_que_dsp_to_ui.readyBytes())
		{
			onQueDataAvailable();
		}
	}
#endif

	// IAudioPluginHost
	gmpi::ReturnCode setPin(int32_t timestamp, int32_t pinId, int32_t size, const uint8_t* data) override;
	gmpi::ReturnCode setPinStreaming(int32_t timestamp, int32_t pinId, bool isStreaming) override;
	gmpi::ReturnCode setLatency(int32_t latency) override;
	gmpi::ReturnCode sleep() override;
	int32_t getBlockSize() override;
	float getSampleRate() override;
	int32_t getHandle() override;
    
	void setHostControlFromDaw(gmpi::hosting::HostControls hc, double value);

protected:
	void CommunicationProc();
	void MidiIn(int sampleOffset, const uint8_t* data, int32_t size);
	void DoNoteOff(int channel, int32_t noteId, float velocity, int sampleOffset);

	struct vstNoteInfo
	{
		int32_t noteId;		// VST3 note ID
		float pitch2;		// in MIDI semitones
		uint8_t channel;
		uint8_t MidiKeyNumber;
		bool held;
	};

	vstNoteInfo* findKey(uint8_t channel, int noteId);

	SeProcessor::vstNoteInfo& allocateKey(const Steinberg::Vst::NoteOnEvent& note);

//	gmpi::shared_ptr<gmpi::api::IProcessor> plugin_;
	gmpi::hosting::gmpi_processor plugin;

	gmpi_dynamic_linking::DLL_HANDLE plugin_dllHandle = {};
	gmpi_dynamic_linking::DLL_HANDLE plugin_dllHandle_to_unload = {};

	bool active_;
//	Steinberg::Vst::ProcessContext timeInfo{};

	std::vector<float*> inputBuffers;
	std::vector<float*> outputBuffers;
	bool outputsAsStereoPairs;

	// Background communication thread.
    std::thread background;
    std::mutex backgroundMutex;
    std::condition_variable backgroundSignal;
	std::atomic<bool> killBackgroundthread = {};

	// MIDI 2.0 allocation
	vstNoteInfo noteIds[16][128] = {};
	int noteIdsRoundRobin = -1;

	gmpi::hosting::lock_free_fifo m_message_que_dsp_to_ui;
	gmpi::hosting::interThreadQue m_message_que_ui_to_dsp;

	// MIDI output
	void MidiToHost(class MidiBuffer3* mb, timestamp_t SeStartClock, int numSamples);
	gmpi::midi_2_0::MidiConverter2 midiConverter;
	float midi2NoteTune[256];
	uint8_t midi2NoteToKey[256];
	struct avoidRepeatedCCs
	{
		float unquantized;
		uint8_t quantized;
	};
	avoidRepeatedCCs ControlChangeValue[128];
	Steinberg::Vst::ProcessData* dataptr = {};

//	std::unordered_map<int32_t, int32_t> param2pin;
	std::vector<float> silence;
	gmpi::hosting::pluginInfo const& info;
	int MidiInputPinIdx = -1;

//	GMPI_QUERYINTERFACE_METHOD(gmpi::api::IAudioPluginHost);
	gmpi::ReturnCode queryInterface(const gmpi::api::Guid* iid, void** returnInterface) override
	{
		*returnInterface = 0;
		if ((*iid) == gmpi::api::IProcessorHost::guid || (*iid) == gmpi::api::IUnknown::guid)
		{
			*returnInterface = static_cast<gmpi::api::IProcessorHost*>(this); GmpiBaseClass::addRef();
			return gmpi::ReturnCode::Ok;
		}
		return gmpi::ReturnCode::NoSupport;
	}
};

}

#endif
