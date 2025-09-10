#pragma
#ifndef __SEInstrumentBase__
#define __SEInstrumentBase__

#include <vector>
#include <map>
#include <mutex>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioUnitUtilities.h>
#include "AudioUnitSDK/AUMIDIBase.h"
#include "wrapper/common/MpParameter.h"
#include "GmpiMidi.h"
#include "Hosting/xml_spec_reader.h"
#include "Hosting/processor_holder.h"
#include "Hosting/message_queues.h"
#include "Hosting/controller_holder.h"

#if 0
#include "SynthRuntime.h"
#include "UMidiBuffer2.h"
//#include "IGuiHost.h"
#include "Controller.h"
#include "ProcessorStateManager.h"
#include "mp_midi.h"
#include "mfc_emulation.h"
#endif

struct MidiEvent3
{
    int64_t timestamp;
    int size;
    unsigned char data[10]; // 10 is just token value.
};

class MidiBuffer3
{
    static const int nullentry = -9999;
    enum { BUFFER_SIZE = 1024}; // * 4 bytes. power-or-two please.
    static const int bufferwrapper = BUFFER_SIZE-1;

    std::vector<int32_t> buffer;
    size_t m_read_pos;
    size_t m_write_pos;

    inline void UpdateReadPosImp()
    {
        auto e = (MidiEvent3*)&buffer[m_read_pos];
        auto totalSize = (sizeof(e->timestamp) + sizeof(e->size) + e->size + sizeof(int32_t) - 1) / sizeof(int32_t);

        m_read_pos = (m_read_pos + totalSize) & bufferwrapper;
    }

public:
    MidiBuffer3() :
        m_read_pos(0)
        ,m_write_pos(0)
    {
        buffer.assign(BUFFER_SIZE, 0);
    }

    void Clear()
    {
        m_read_pos = m_write_pos = 0;
    }

    inline void Add(int64_t timestamp, const unsigned char* data, int size)
    {
        assert(m_write_pos >= 0 && m_write_pos < buffer.size());
        const size_t sizeof_header = sizeof(timestamp) + sizeof(int32_t);

        if (size + sizeof_header > FreeSpace() * sizeof(int32_t))
        {
            // _RPT0(_CRT_WARN, "OVERLOAD on MIDI input!!!!\n");
            return;
        }

        // check if enough space before end of buffer.
        auto e = (MidiEvent3*)&buffer[m_write_pos];
        auto totalSize = (sizeof(e->timestamp) + sizeof(e->size) + size + sizeof(int32_t) - 1) / sizeof(int32_t);
        if (m_write_pos + totalSize > buffer.size())
        {
            // skip remainder of buffer and wrap.
            Add(nullentry, reinterpret_cast<const unsigned char*>( buffer.data() ), (int) (sizeof(int32_t) * (buffer.size() - m_write_pos) - sizeof_header));

            Add(timestamp, data, size);
            return;
        }

        e->timestamp = timestamp;
        e->size = size;

        auto dest = e->data;
        for (int i = 0; i < size; i++)
        {
            *dest++ = data[i];
        }

        // commit.
        m_write_pos = (m_write_pos + totalSize) & bufferwrapper;
    }

    inline bool IsEmpty()
    {
        auto isempty = (m_read_pos == m_write_pos);
        if (!isempty)
        {
            auto e = (MidiEvent3*)&buffer[m_read_pos];

            if (e->timestamp == nullentry)
            {
                UpdateReadPosImp();
                return IsEmpty();
            }
        }

        return isempty;
    }

    inline int64_t PeekNextTimestamp()
    {
        if (IsEmpty())
        {
            return 0;
        }

        return *(int64_t*)&buffer[m_read_pos];
    }

    inline MidiEvent3* Current()
    {
        assert(!IsEmpty());
        auto e = (MidiEvent3*)&buffer[m_read_pos];

        if (e->timestamp == nullentry)
        {
            UpdateReadPosImp();
            assert(!IsEmpty());
            return Current();
        }
        return e;
    }

    inline void UpdateReadPos()
    {
        assert(!IsEmpty());
        UpdateReadPosImp();
    }

    inline size_t FreeSpace()
    {
        if (m_read_pos > m_write_pos)
        {
            return m_read_pos - m_write_pos - 1;
        }

        return m_read_pos + buffer.size() - m_write_pos - 1;
    }
};

struct parameterChange
{
	UInt32					BufferOffsetInFrames;
	AudioUnitParameterID	ID;
	AudioUnitParameterValue	Value;
};

#if 1
class MpParameterAU : public wrapper::MpParameter_native
{
	bool isInverted = {};
	class SEInstrumentBase* AUcontroller = {};
	AudioUnitParameter nativeParameter_;
	std::atomic<float> dawFacingValueReal = {}; // cache latest update from Ableton, least Ableton queries it while it's queued.

public:

	MpParameterAU(class SEInstrumentBase* controller, AudioUnitParameter nativeParameter, bool isInverted);

    int getNativeTag() override
    {
        return nativeParameter_.mParameterID;
    }
    
	void setRealFromDaw(double value)
	{
        //_RPT1(0,"setRealFromDaw %f\n", value);
		const auto normalised_se = static_cast<float>(RealToNormalized(value));
        setParameterRaw(gmpi::Field::Normalized, sizeof(normalised_se), &normalised_se);
	}

	void setValueImmediate(float value)
	{
     //   _RPT1(0,"setValueImmediate %f\n", value);
		dawFacingValueReal.store(value, std::memory_order_release);
	}
	float getValueImmediate() const
	{
        const auto value = dawFacingValueReal.load(std::memory_order_relaxed);
 //       _RPT1(0,"getValueImmediate %f\n", value);

        return value;
	}

	// AU host (e.g. Live) can que parameter changes which don't take affect immediatly,
	// but expect to query current value immediatly. Cache latest value here (only for reporting to DAW).
	void upDateImmediateValue() override
	{
    //    _RPT0(0,"upDateImmediateValue=> ");
		setValueImmediate(getValueReal());
	}

	void updateProcessor(gmpi::Field filedId, int32_t voice) override;

	float convertNormalized(float value) const
	{
		return isInverted ? 1.0f - value : value;
	}
    void updateDawUnsafe(const std::string& rawValue) override {}
};
#endif


class SEInstrumentBase : public ausdk::AUBase, public ausdk::AUMIDIBase
, public gmpi::api::IProcessorHost, public gmpi::TimerClient
// public MpController, public IShellServices, public IProcessorMessageQues //, public IAuGui
{
	friend class MpParameterAU;
    static const int timerPeriodMs = 35;
	gmpi::hosting::gmpi_processor plugin;
	gmpi::hosting::gmpi_controller_holder gmpiController;

	std::vector<parameterChange> parameterChanges[2];
	int latencyCompensation; // enum.
	bool wantsMidi = false;
	std::vector<float*> outputPtr;
	std::vector<float*> inputPtr;
	bool outputsAsStereoPairs = true;
	bool monoUseOk = false;

	std::vector<AUChannelInfo> busArrangement;
	std::vector<AUChannelInfo> supportedChannels;

	AudioUnitCocoaViewInfo cocoaInfo; // custom GUI class information.
	std::map<std::string, std::vector<CFStringRef> > enumStrings; // cache of native enum lists
//	std::map<int, MpParameterAU*> tagToParameter;
	UInt32 offLineRenderMode = 0;
	gmpi::hosting::interThreadQue queueToDsp_;
	gmpi::midi_2_0::MidiConverter2 midiConverter;
//	gmpi::midi_2_0::MpeConverter mpeConverter;
//    int userNotHoldingAControlCounter = 0;
    
//	ProcessorStateMgr stateMgr;
//	gmpi::hosting::interThreadQue message_que_dsp_to_ui;

#ifdef _DEBUG
    std::thread::id mainThreadID;
#endif
#if 0
	// MPE
	int lower_zone_size = 0; // both 0 = not MPE mode
	int upper_zone_size = 0;
	bool auto_mpe_mode = false;

	unsigned short incoming_rpn[16];
	// RPNs are 14 bit values, so this value never occurs, represents "no rpn"
	static const unsigned short NULL_RPN = 0xffff;
	void cntrl_update_msb(unsigned short& var, short hb) const
	{
		var = (var & 0x7f) + (hb << 7);	// mask off high bits and replace
	}
	void cntrl_update_lsb(unsigned short& var, short lb) const
	{
		var = (var & 0x3F80) + lb;			// mask off low bits and replace
	}
#endif

	void reInitialize();
protected:
	AUEventListenerRef mParameterListener;
	bool onTimer() override;

public:
	SEInstrumentBase(AudioComponentInstance	inInstance);
	virtual ~SEInstrumentBase();

	// IShellServices
	void onQueDataAvailable() {}
	void flushPendingParameterUpdates();
	void EnableIgnoreProgramChange()
	{
//		stateMgr.enableIgnoreProgramChange();
	}

	virtual void                PostConstructor() override;
	virtual OSStatus			Initialize() override;

	virtual OSStatus            SaveState(CFPropertyListRef* outData) override;
	virtual OSStatus            RestoreState(CFPropertyListRef inData) override;


	/*! @method Parts */
	ausdk::AUScope& Parts() { return mPartScope; }

	/*! @method GetPart */
	ausdk::AUElement* GetPart(AudioUnitElement inElement)
	{
		return mPartScope.SafeGetElement(inElement);
	}

	virtual ausdk::AUScope* GetScopeExtended(AudioUnitScope inScope) override;

	virtual void				CreateExtendedElements() override;

	virtual OSStatus			Reset(AudioUnitScope 					inScope,
		AudioUnitElement 				inElement) override;

	bool ValidFormat(AudioUnitScope inScope, AudioUnitElement inElement,
		const AudioStreamBasicDescription& inNewFormat) override;

	virtual UInt32              SupportedNumChannels(const AUChannelInfo** outInfo) override;

	virtual bool				StreamFormatWritable(AudioUnitScope					scope,
		AudioUnitElement				element) override;

	virtual bool				CanScheduleParameters() const override { return false; }

	virtual OSStatus			Render(AudioUnitRenderActionFlags& ioActionFlags,
		const AudioTimeStamp& inTimeStamp,
		UInt32							inNumberFrames) override;

	// MIDI dispatch
//	OSStatus MIDIEvent(
//		UInt32 inStatus, UInt32 inData1, UInt32 inData2, UInt32 inOffsetSampleFrame) override;
        
#if AUSDK_HAVE_MIDI2
	OSStatus MIDIEventList(
		UInt32 /*inOffsetSampleFrame*/, const struct MIDIEventList* /*eventList*/) override;
#endif

    void ParamGrabbed(wrapper::MpParameter_native* param) 
	{
 //       _RPT2(0,"ParamGrabbed(%d) %d\n", (int) param->isGrabbed(), param->getNativeTag());

		AudioUnitEvent e;
		e.mArgument.mParameter.mAudioUnit = GetComponentInstance();
		e.mArgument.mParameter.mParameterID = param->getNativeTag();
		e.mArgument.mParameter.mScope = kAudioUnitScope_Global;
		e.mArgument.mParameter.mElement = 0;

		if (param->isGrabbed())
		{
			e.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
//            userNotHoldingAControlCounter = std::numeric_limits<int>::max();
		}
		else
		{
			e.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
//            userNotHoldingAControlCounter = 4; // short delay before resuming updates from the processor (to avoid jitter)
		}

		AUEventListenerNotify(mParameterListener, NULL, &e);
	}
    
#if 0 // test, no improvement in Logic Pro touch automation, issues in Ableton with param not getting to DSP
    // notify DAW that parameter changed from the UI
    void ParamChanged(MpParameter_native* param)
    {
        AudioUnitEvent e;
        e.mArgument.mParameter.mAudioUnit = GetComponentInstance();
        e.mArgument.mParameter.mParameterID = param->getNativeTag();
        e.mArgument.mParameter.mScope = kAudioUnitScope_Global;
        e.mArgument.mParameter.mElement = 0;
        e.mEventType = kAudioUnitEvent_ParameterValueChange;

        AUEventListenerNotify(mParameterListener, NULL, &e);
    }
#endif

#if 0
	int32_t getController(int32_t handle, gmpi::IMpController** returnController) override
	{
		return 0;
	}
#endif
    
	virtual OSStatus 	SetParameter(AudioUnitParameterID			inID,
		AudioUnitScope 					inScope,
		AudioUnitElement 				inElement,
		AudioUnitParameterValue			inValue,
		UInt32							inBufferOffsetInFrames) override;

	OSStatus 	GetParameter(AudioUnitParameterID			inID,
		AudioUnitScope 					inScope,
		AudioUnitElement 				inElement,
		AudioUnitParameterValue& outValue) override;

	virtual OSStatus            GetParameterValueStrings(AudioUnitScope                 inScope,
		AudioUnitParameterID            inParameterID,
		CFArrayRef* outStrings) override;

	static void ParameterListener(void* inCallbackRefCon, void* inObject, const AudioUnitEvent* inEvent, UInt64 inEventHostTime, Float32 inParameterValue);
//	// IAuGui interface
//	void OnParameterUpdateFromDaw(int32_t tag, float normalised) override;

#if 0
    wrapper::MpParameter_native* makeNativeParameter(int ParameterIndex, bool isInverted)
	{
		AudioUnitParameter sPar = { GetComponentInstance(), static_cast<AudioUnitParameterID>(ParameterIndex), kAudioUnitScope_Global, 0 };

		auto param = new MpParameterAU(this, sPar, isInverted);

		tagToParameter.insert(std::make_pair(ParameterIndex, param));

		return param;
	}

    wrapper::MpParameter* getDawParameter(int nativeTag)
	{
		auto it = tagToParameter.find(nativeTag);
		if (it != tagToParameter.end())
		{
			return (*it).second;
		}
		return {};
	}
#endif
    
    gmpi::hosting::IWriteableQue* getQueueToDsp() //override
	{
		return &queueToDsp_;
	}

	// IProcessorMessageQues
  //  gmpi::hosting::IWriteableQue* MessageQueToGui() //override
	//{
	//	return &message_que_dsp_to_ui;
	//}
    void Service()  {} // VST3 only.
    gmpi::hosting::interThreadQue* ControllerToProcessorQue() //override
	{
		return &queueToDsp_;
	}

	std::function<void(void)> callbackOnUnloadPlugin;

    float sampleRate{44100.f};
    
    // IProcessorHost
    gmpi::ReturnCode setPin(int32_t timestamp, int32_t pinId, int32_t size, const uint8_t* data) override;
    gmpi::ReturnCode setPinStreaming(int32_t timestamp, int32_t pinId, bool isStreaming) override;
    gmpi::ReturnCode setLatency(int32_t latency) override;
    gmpi::ReturnCode sleep() override;
    int32_t getBlockSize() override;
    float getSampleRate() override;
    int32_t getHandle() override;
    
protected:

	void				PerformEvents(const AudioTimeStamp& inTimeStamp);

	OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
		AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable) override;

	virtual OSStatus			GetProperty(AudioUnitPropertyID 	inID,
		AudioUnitScope 			inScope,
		AudioUnitElement 		inElement,
		void* outData) override;

	virtual OSStatus            SetProperty(AudioUnitPropertyID             inID,
		AudioUnitScope                  inScope,
		AudioUnitElement                inElement,
		const void* inData,
		UInt32                          inDataSize) override;

	OSStatus GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID,
		AudioUnitParameterInfo& outParameterInfo) override;

	Float64 GetLatency() override
	{
        return 0;//processor.getLatencySamples() / timeInfo.sampleRate;
	}
    void OnLatencyChanged() 
    {
        PropertyChanged(kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0);
    }

	// Presets
    void setPresetXmlFromSelf(const std::string& xml); //override;
 //   void setPresetFromSelf(DawPreset const* preset); //override;
	virtual void OnStartPresetChange()  {};
	virtual void OnEndPresetChange()  {};
	std::wstring getNativePresetExtension()
	{
		return L"aupreset";
	}
    void saveNativePreset(const char* filename, const std::string& presetName, const std::string& xml) ;
    std::string loadNativePreset(std::wstring sourceFilename) ;
//    std::vector< wrapper::MpController::presetInfo > scanFactoryPresets() override { return {}; }
//	void loadFactoryPreset(int index, bool fromDaw) override {};
    void onSetParameter(int32_t handle, int32_t field, RawView rawValue, int voiceId)  {}; // VST3 Only
    
    std::string getFactoryPresetXml(std::string filename)  {return {};} // JUCE-only?
    
	SInt64 mAbsoluteSampleFrame;

private:
	// double-buffered incoming MIDI events.
	MidiBuffer3 midiEvents[2];
	std::atomic<int> curMidiEvents;

	ausdk::AUScope	mPartScope;
	const UInt32	mInitNumPartEls;

	std::mutex hostMidiLock;
	float dummyInputBuffer[kAUDefaultMaxFramesPerSlice];
	float dummyOutputBuffer[kAUDefaultMaxFramesPerSlice];

//	std::string pluginType; // "aumu" : "aufx"
//	std::string manufacturerId;

//    uint8_t midi2conversionbuffer[256];
    bool processorIsInitialized = false;
    
    GMPI_REFCOUNT_NO_DELETE;
    gmpi::ReturnCode queryInterface(const gmpi::api::Guid* iid, void** returnInterface) override
    {
        *returnInterface = 0;
        if ((*iid) == gmpi::api::IProcessorHost::guid || (*iid) == gmpi::api::IUnknown::guid)
        {
            *returnInterface = static_cast<gmpi::api::IProcessorHost*>(this); addRef();
            return gmpi::ReturnCode::Ok;
        }
        return gmpi::ReturnCode::NoSupport;
    }
};

#endif

