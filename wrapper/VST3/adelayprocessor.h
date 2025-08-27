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
#include "wrapper/common/lock_free_fifo.h"
#include "wrapper/common/interThreadQue.h"
#include "wrapper/common/dynamic_linking.h"
#include "wrapper/common/HostControls.h"

static const int MidiControllersParameterId = 10000;

enum class VoiceAllocationHint {Keyboard, MPE};
struct pluginInfoSem;

namespace wrapper
{

// to work around Steinberg Interfaces having incompatible addRef etc
class GmpiBaseClass :public gmpi::api::IProcessorHost
{
public:
	GMPI_REFCOUNT_NO_DELETE;
};

template<int N>
class EventQue
{
	std::array<gmpi::api::Event, N> events;
	int tail = 0;
public:

	EventQue()
	{
		for (size_t i = 0 ; i < events.size() - 1; ++i)
		{
			events[i].next = &events[i + 1];
		}
	}

	void push(gmpi::api::Event event)
	{
		assert(tail != events.size()); // opps, filled event list up.

		if (tail < events.size() - 1)
		{
			//if(tail > 0)
			//	events[tail - 1].next = &(events[tail]);

			events[tail++] = event;
		}
	}

	gmpi::api::Event* head()
	{
		if (tail == 0)
			return {};

		// create linked list.
		for (int i = 0; i < tail - 1; ++i)
		{
			events[i].next = &events[i + 1];
		}

		events[tail - 1].next = {};
		return &events[0];
	}

	void clear()
	{
		tail = 0;
	}
};

//-----------------------------------------------------------------------------
typedef int64_t timestamp_t;

struct DawParameter : public QueClient // also host-controls, might need to rename it.
{
	int32_t id{};
	double valueReal = 0.0;
	double valueLo = 0.0;
	double valueHi = 1.0;

	bool setNormalised(double value)
	{
		const auto newValueReal = valueLo + value * (valueHi - valueLo);

		const bool r = newValueReal != valueReal;

		valueReal = newValueReal;

		return r;
	}

	bool setReal(double value)
	{
		const bool r = value != valueReal;

		valueReal = value;

		return r;
	}

	double normalisedValue() const
	{
		if (valueHi == valueLo)
			return 0.0; // avoid divide by zero.
		return (valueReal - valueLo) / (valueHi - valueLo);
	}

	int queryQueMessageLength(int availableBytes) override
	{
		return sizeof(double);
	}

	void getQueMessage(class my_output_stream& outStream, int messageLength) override
	{
		const bool hostNeedsParameterUpdate{};
		const int32_t voice{};

		outStream << id;
		outStream << id_to_long("ppc2");
		outStream << messageLength;

		outStream << valueReal;
	}
};

class PatchManager
{
public:
	std::unordered_map<int, DawParameter> parameters;

	PatchManager() = default;

	DawParameter* getParameter(int id)
	{
		if (auto it = parameters.find(id) ; it != parameters.end())
			return &(it->second);

		return {};
	}

	// return the parameter only if it changed.
	DawParameter* setParameterNormalised(int id, double value)
	{
		auto it = parameters.find(id);
		if (it == parameters.end())
			return {};

		auto& param = it->second;

		if(param.setNormalised(value))
			return &param;
	}

	// return the parameter only if it changed.
	DawParameter* setParameterReal(int id, double value)
	{
		auto it = parameters.find(id);
		if (it == parameters.end())
			return {};

		auto& param = it->second;

		if (value == param.valueReal)
			return {};

		param.valueReal = value;
		return &param;
	}
};

class SeProcessor : public Steinberg::Vst::AudioEffect, public GmpiBaseClass //, public IShellServices, public IProcessorMessageQues
{
public:
	SeProcessor (pluginInfoSem& pinfo);
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
	IWriteableQue* MessageQueToGui() // override
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
	void setHostControlFromDaw(wrapper::HostControls hc, double value);

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

	gmpi::shared_ptr<gmpi::api::IProcessor> plugin_;

	EventQue<1000> events;

	gmpi_dynamic_linking::DLL_HANDLE plugin_dllHandle = {};
	gmpi_dynamic_linking::DLL_HANDLE plugin_dllHandle_to_unload = {};

	bool active_;
//	Steinberg::Vst::ProcessContext timeInfo{};

	PatchManager patchManager;

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

    lock_free_fifo m_message_que_dsp_to_ui;
	interThreadQue m_message_que_ui_to_dsp;

	// Communication pipes Controller<->Processor
	QueuedUsers pendingControllerQueueClients; // parameters waiting to be sent to GUI

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
	pluginInfoSem const& info;
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
