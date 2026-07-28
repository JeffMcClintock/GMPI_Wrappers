#include <iostream>
#include <dlfcn.h>
#include "AU2_Wrapper.h"
#include "Hosting/xml_spec_reader.h"
#include "Hosting/gmpi_factory.h"
#include "GmpiSdkCommon.h"

using namespace ausdk;

#ifdef DEBUG
#define DEBUG_PRINT 0
#define DEBUG_PRINT_NOTE 0
#define DEBUG_PRINT_RENDER 0
#endif

// provide extensibility to add extra modules on a per-project basis.
// SE2JUCE Controller must implement this function
extern void initialise_synthedit_extra_modules(bool passFalse)
{
	// here to satisfy linker
}

#if 0
MpParameterAU::MpParameterAU(AU2_Wrapper* controller, AudioUnitParameter nativeParameter, bool isinverted) :
MpParameter_native({})//controller)
	, AUcontroller(controller)
	, nativeParameter_(nativeParameter)
	, isInverted(isinverted)
{
}

void MpParameterAU::updateProcessor(gmpi::Field fieldId, int32_t voice)
{
    switch(fieldId)
    {
        case gmpi::Field::Grab:
            // AUcontroller->ParamGrabbed(this);
            onGrabbedChanged(); // combine MIDI-grab-emulation and mouse grab into unified grab messages
            break;
            
        case gmpi::Field::Normalized:
        case gmpi::Field::Value:
        {
            const auto value = getValueReal();
#ifdef _DEBUG
    std::cerr << "DAW <= AUParameterSet(" << value << ")" << std::endl;
#endif
            upDateImmediateValue();
#if 0
            AUcontroller->ParamChanged(this); // flaky in Ableton, SE UI wasn't updating DSP
#else
            // we pass the parameter listener to *prevent* notifying the GUI via AU2_Wrapper::ParameterListener(). otherwise we get jittery controls.
            AUParameterSet(
                AUcontroller->mParameterListener,
                NULL, //AUcontroller->GetComponentInstance(), //this, //NULL,
                &nativeParameter_,
                value,
                0
            );
 /* not helpful for Logic Pro red flashes
  
            // copied from VST3 SDK wrapper, call both AUParameterSet AND AUEventListenerNotify
            AudioUnitEvent e;
            e.mArgument.mParameter.mAudioUnit = AUcontroller->GetComponentInstance();
            e.mArgument.mParameter.mParameterID = getNativeTag();
            e.mArgument.mParameter.mScope = kAudioUnitScope_Global;
            e.mArgument.mParameter.mElement = 0;
            e.mEventType = kAudioUnitEvent_ParameterValueChange;

            AUEventListenerNotify(AUcontroller->mParameterListener, NULL, &e);
 */
#endif
        }
        break;
            
        default:
            break;
    }
}
#endif

AU2_Wrapper::AU2_Wrapper(AudioComponentInstance inInstance)
	: AUBase(inInstance, 0, 0, 1),
    AUMIDIBase(*static_cast<AUBase*>(this)),
	mParameterListener(nullptr),
	mAbsoluteSampleFrame(0),
	curMidiEvents(0),
	mInitNumPartEls(1),
	latencyCompensation(0),
	queueToDsp_(0x500000)//SeAudioMaster::AUDIO_MESSAGE_QUE_SIZE),
//    message_que_dsp_to_ui(0x500000)

	,midiConverter(
		// provide a lambda to accept converted MIDI 2.0 messages
		[this](const gmpi::midi2::message_view& msg, int sampleOffset) {
            gmpi::api::Event ge
            {
                {},									// next (populated later)
                sampleOffset,						// timeDelta
                gmpi::api::EventType::Midi,
                plugin.MidiInputPinIdx,				// pinIdx
                static_cast<int32_t>(msg.size()),	// size_
                {}									// data_/oversizeData_
            };

            auto dst = reinterpret_cast<uint8_t*>(&ge.data_);
            auto data = msg.begin();
            std::copy(data, data + msg.size(), dst);

            plugin.events.push(ge);
		}
	)
#if 0
	, mpeConverter(
		// provide a lambda to accept converted MIDI 2.0 messages
		[this](const gmpi::midi::message_view& msg, int timestamp) {
//			processor.MidiIn(timestamp, (const unsigned char*)msg.begin(), static_cast<int>(msg.size()));
		}
	)
#endif
{
	parameterChanges[0].reserve(200);
	parameterChanges[1].reserve(200);
    
    gmpiController.notifyDaw = [this](gmpi::hosting::GmpiParameter const* param)
        {
            assert(param->info->dawTag != -1); // should never be called for non-native param.

            const auto paramID = param->info->dawTag;

            // Usually parameter will have sent beginEdit() already (if it has mouse-down connected properly, else fake it.
//            if (!param->isGrabbed)
 //               beginEdit(paramID);

            //   _RPT2(0, "param[%d] %f => DAW\n", paramID, param->getNormalized());
            //performEdit(paramID, param->normalisedValue()); // Send the value to DSP.
            
            // we pass the parameter listener to *prevent* notifying the GUI via AU2_Wrapper::ParameterListener(). otherwise we get jittery controls.
            AudioUnitParameter nativeParameter;
            nativeParameter.mAudioUnit = GetComponentInstance();
            nativeParameter.mParameterID = paramID;
            nativeParameter.mScope = kAudioUnitScope_Global;
            nativeParameter.mElement = 0;
            
            AUParameterSet(
                mParameterListener,
                NULL, //AUcontroller->GetComponentInstance(), //this, //NULL,
                &nativeParameter,
                param->valueReal(),
                0
            );

 //           if (!param->isGrabbed)
  //              endEdit(paramID);
        };

	auto& info = *gmpi::hosting::factory::getInstance().getPluginInfo();
    plugin.init(info);
	gmpiController.init(info);

//	memset(&timeInfo, 0, sizeof(timeInfo));

//	processor.connectPeer(this);

#if DEBUG_PRINT
	printf("new AU2_Wrapper\n");
#endif
    
//    fprintf(stderr, "AU WRAPPER CONSTRUCTOR!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
	SetWantsRenderThreadID(true);
}

void AU2_Wrapper::PostConstructor()
{
#ifdef _DEBUG
    mainThreadID = std::this_thread::get_id();
#endif
    
	memset(&dummyInputBuffer, 0, sizeof(dummyInputBuffer));

	// Call AUbase create elements purely so it wil set it's flag
	// saying they are initialised so it won't override my busses later.
	CreateElements();

	int inputCount = 0;
	int outputCount = 0;
	std::vector<std::string> outputNames;

	// Determine number of inputs and outputs here.
	{
        auto& info = *gmpi::hosting::factory::getInstance().getPluginInfo();

        inputCount  = countPins(info, gmpi::PinDirection::In , gmpi::PinDatatype::Audio);
        outputCount = countPins(info, gmpi::PinDirection::Out, gmpi::PinDatatype::Audio);
            
        inputPtr.assign(inputCount, nullptr);
        outputPtr.assign(outputCount, nullptr);

        int i = 0;
        for(auto& pin : info.dspPins)
        {
            if(pin.datatype == gmpi::PinDatatype::Audio && pin.direction == gmpi::PinDirection::Out)
                outputNames.push_back(pin.name);
            
            if(pin.datatype == gmpi::PinDatatype::Midi && pin.direction == gmpi::PinDirection::In)
            {
                wantsMidi = true;
                plugin.MidiInputPinIdx = i;
            }
            ++i;
        }
    }
	
	// Can't call base because num elements (busses) not avail in contructor. Do it myself.
	// MusicDeviceBase::PostConstructor();
	{
		// outputsAsStereoPairs =false;

		// default plugin channel configuration.
//        supportedChannels.push_back({static_cast<SInt16>(inputCount), static_cast<SInt16>(outputCount)});

		// we have either no input bus, or one, depending if we have any input channels.
		const int maxChannelsPerBus = outputsAsStereoPairs ? 2 : std::max(inputCount, outputCount);
		{
			int inputBusCount = (maxChannelsPerBus - 1 + inputCount) / maxChannelsPerBus;
			int outputBusCount = (maxChannelsPerBus - 1 + outputCount) / maxChannelsPerBus;
			Inputs().Initialize(this, kAudioUnitScope_Input, inputBusCount);
			Outputs().Initialize(this, kAudioUnitScope_Output, outputBusCount);

			// AFTER we have created elements, set default number of channels on each.
			// Set default channel configuration reported to AUVAL. "VERIFYING DEFAULT SCOPE FORMATS" phase.
			int channelsRemain = inputCount;
			int element = 0;

			while (channelsRemain > 0)
			{
				if (busArrangement.size() <= element)
				{
					busArrangement.push_back(AUChannelInfo());
					busArrangement[element].outChannels = 0;
				}

				int channels = (std::min)(maxChannelsPerBus, channelsRemain);
				busArrangement[element].inChannels = channels;
				auto io = Inputs().GetIOElement(element);
				auto defaultStreamFormat = io->GetStreamFormat();
				defaultStreamFormat.mChannelsPerFrame = channels; //   .ChangeNumberChannels(channels, false);
				io->SetStreamFormat(defaultStreamFormat);

				channelsRemain -= channels;
				++element;
			}

			channelsRemain = outputCount;
			element = 0;
			int outputIdx = 0;
			while (channelsRemain > 0)
			{
				if (busArrangement.size() <= element)
				{
					busArrangement.push_back(AUChannelInfo());
					busArrangement[element].inChannels = 0;
				}
				int channels = (std::min)(maxChannelsPerBus, channelsRemain);
				busArrangement[element].outChannels = channels;
				auto io = Outputs().GetIOElement(element);
				auto defaultStreamFormat = io->GetStreamFormat();

				defaultStreamFormat.mChannelsPerFrame = channels; //.ChangeNumberChannels(channels, false);
				io->SetStreamFormat(defaultStreamFormat);

				if (outputNames.size() >= outputIdx + channels)
				{
					std::string outputBusName;
					if (2 == channels)
					{
						// calc common part of output names
						const auto& l = outputNames[outputIdx];
						const auto& r = outputNames[outputIdx + 1];
						for (int i = 0; i < std::min(l.size(), r.size()); ++i)
						{
							if (l[i] == r[i])
							{
								outputBusName.push_back(l[i]);
							}
						}
					}
					else
					{
						outputBusName = outputNames[outputIdx];
					}

					if (!outputBusName.empty())
					{
						CFStringRef cfStr = CFStringCreateWithCString(NULL, outputBusName.c_str(), kCFStringEncodingUTF8);
						if (cfStr)
						{
							io->SetName(cfStr);
							CFRelease(cfStr);
						}
					}
				}

				channelsRemain -= channels;
				outputIdx += channels;
				++element;
			}
            
            if (monoUseOk && inputCount == 2 && outputCount == 2)
            {
                 //supportedChannels[0].inChannels = -1; // [-1,2] means any number of inputs with 2 outs.
                supportedChannels.push_back({-1, 2 });
                supportedChannels.push_back({ 1, 1 });
            }
            else
            {
                // 'supportedChannels' should I think contain one entry for each bus, specifying the ins and outs of that bus.
                for (auto& element : busArrangement)
                {
                    supportedChannels.push_back({ element.inChannels, element.outChannels });
                }
            }
		}
	}
#if 0
    // STATE MANAGER
    {
        stateMgr.callback =
            [this](const DawPreset* preset)
            {
                processor.setPresetUnsafe(preset); // set preset on processor
				//setPresetFromDaw(chunk, false);    // set preset on controller
				setPreset(preset);
			};

        tinyxml2::XMLDocument doc;
        {
            const auto xml = BundleInfo::instance()->getResource("parameters.se.xml");
            doc.Parse(xml.c_str());
            assert(!doc.Error());
        }

        auto controllerE = doc.FirstChildElement("Controller");
        assert(controllerE);

        auto patchManagerE = controllerE->FirstChildElement();
        assert(strcmp(patchManagerE->Value(), "PatchManager") == 0);

        std::wstring_convert<std::codecvt_utf8<wchar_t>> convert;

        auto parameters_xml = patchManagerE->FirstChildElement("Parameters");

        stateMgr.init(parameters_xml);
    }
    
    MpController::Initialize();

	ScanPresets();
#endif
    // Create Native Parameters.
    for (auto p : gmpiController.nativeParams)
    {
 //       auto p = it.second;
        Globals()->SetParameter(p->info->dawTag, p->valueReal());
    }

    auto result = AUEventListenerCreate(ParameterListener,
        this,
        CFRunLoopGetCurrent(),
        kCFRunLoopDefaultMode,
        0.02,
        0.001,
        &mParameterListener);
    
    // Subscribe to parameter-change notifications, to get updates from host.
    // must be done in main thread, otherwise automation is very weird glitchy
    {
        AudioUnitEvent myEvent;

        myEvent.mArgument.mParameter.mAudioUnit = GetComponentInstance();
        myEvent.mArgument.mParameter.mElement = 0;
        myEvent.mArgument.mParameter.mScope = kAudioUnitScope_Global;

        for (const auto p : gmpiController.nativeParams)
        {
            myEvent.mArgument.mParameter.mParameterID = p->info->dawTag;
            
            myEvent.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
            auto result = AUEventListenerAddEventType(mParameterListener, this, &myEvent);
            
            myEvent.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
            result = AUEventListenerAddEventType(mParameterListener, this, &myEvent);
            
            myEvent.mEventType = kAudioUnitEvent_ParameterValueChange;
            result = AUEventListenerAddEventType(mParameterListener, this, &myEvent);
        }
    }
#if 0
    {
        // DAW may not restore preset on first use.
        // but we still need to sync the preset controls, esp so that blank preset name gets changed to "Default" like on VST3
        auto preset = getPreset();
        setPreset(preset.get());
    }
#endif
        
	startTimer(timerPeriodMs); // Service DSP Queue.
}

AU2_Wrapper::~AU2_Wrapper()
{
    stopTimer();
    
	if (callbackOnUnloadPlugin)
	{
		callbackOnUnloadPlugin();
	}

	AUListenerDispose(mParameterListener);

	for (auto it : enumStrings)
	{
		for (auto cfstring : it.second)
		{
			CFRelease(cfstring);
		}
	}
#if DEBUG_PRINT
	printf("delete AU2_Wrapper\n");
#endif
}

void AU2_Wrapper::CreateExtendedElements()
{
	Parts().Initialize(this, kAudioUnitScope_Part, mInitNumPartEls);
}

AUScope* AU2_Wrapper::GetScopeExtended(AudioUnitScope inScope)
{
	if (inScope == kAudioUnitScope_Part)
		return &mPartScope;
	return NULL;
}

// called on a background thread in Logic Pro
OSStatus AU2_Wrapper::Initialize()
{
	/*
	TO DO:
		Currently ValidFormat will check and validate that the num channels is not being
		changed if the AU doesn't support the SupportedNumChannels property - which is correct

		What needs to happen here is that IFF the AU does support this property, (ie, the AU
		can be configured to have different num channels than its original configuration) then
		the state of the AU at Initialization needs to be validated.

		This is work still to be done - see AUEffectBase for the kind of logic that needs to be applied here
	*/
    sampleRate = Output(0).GetStreamFormat().mSampleRate;
#if 0
	//    GetSampleRate();
	timeInfo.sampleRate = Output(0).GetStreamFormat().mSampleRate;

// don't want on bg thread	MpController::Initialize();

	// construct synth object.
	processor.prepareToPlay(
		this,
		timeInfo.sampleRate,
        GetMaxFramesPerSlice(), //		kAUDefaultMax FramesPerSlice,
		0 == offLineRenderMode
	);

	wantsMidi = processor.wantsMidi();

	mAbsoluteSampleFrame = 0;

	initSemControllers();
#endif

	auto& info = *gmpi::hosting::factory::getInstance().getPluginInfo();

	plugin.start_processor(this, info, GetMaxFramesPerSlice(), Output(0).GetStreamFormat().mSampleRate);

	if (!plugin.processor)
		return 1;

    processorIsInitialized = true;
    
	return noErr;
}

void AU2_Wrapper::reInitialize()
{
	// Reaper may trigger this too early. In which case no need to reinit (since we didn't init yet)
	if (!processorIsInitialized)
		return;

	auto& info = *gmpi::hosting::factory::getInstance().getPluginInfo();

	plugin.start_processor(this, info, GetMaxFramesPerSlice(), Output(0).GetStreamFormat().mSampleRate);

	if (!plugin.processor)
		return;
}

// receive notifications of parameter updates from DAW only (not my own GUI)
void AU2_Wrapper::ParameterListener(void* inCallbackRefCon, void* inObject, const AudioUnitEvent* inEvent, UInt64 inEventHostTime, Float32 inParameterValue)
{
	if (inEvent->mEventType == kAudioUnitEvent_ParameterValueChange)
	{
        auto au = (AU2_Wrapper*)inObject;

        const auto dawTag = inEvent->mArgument.mParameter.mParameterID;
        
        if(dawTag < 0 || dawTag >= au->gmpiController.nativeParams.size())
            return;
        
        auto p = au->gmpiController.nativeParams[dawTag];
        
        if (auto p2 = au->gmpiController.patchManager.setParameterReal(p->info->id, inParameterValue); p2) // todo avoid lookup when we already have pointer to parameter
        {
            au->gmpiController.notifyGui(p2);
        }
#if 1
//		if (auto p = au->getDawParameter(inEvent->mArgument.mParameter.mParameterID); p)
		{
#ifdef _DEBUG
        std::cerr << "DAW => ParameterListener(" << inEvent->mArgument.mParameter.mParameterID << ", " << inParameterValue << ")" << std::endl;
#endif
// TODO			p->setRealFromDaw(inParameterValue);
		}
#endif
	}
}

bool AU2_Wrapper::onTimer()
{
#if 0
    // avoid jitter when the user is moving a control
    if(userNotHoldingAControlCounter < 0)
    {
        stateMgr.queryUpdates(this);
    }
    else
    {
        userNotHoldingAControlCounter--;
    }
 
	return MpController::OnTimer();
#endif

	// parameter updates from the Processor
    gmpiController.message_que_dsp_to_ui.pollMessage(&gmpiController);

	// parameter updates to the Processor
	gmpi::hosting::my_msg_que_output_stream toProcessor(&queueToDsp_);
/*
    if(gmpiController.pendingControllerQueueClients.ServiceWaiters(
		toProcessor,
		queueToDsp_.freeSpace(),
		queueToDsp_.freeSpace()
	))
    {
        queueToDsp_.Send();
    }
*/
    return true;
}

#if 0
// also provides updates on paramters that the GUi is changing
void AU2_Wrapper::OnParameterUpdateFromDaw(int32_t tag, float value)
{
	if (auto p = getDawParameter(tag); p)
	{
        _RPT2(0, "Param to GUI %d %f\n", tag, value);
//		p->setRealFromDaw(value);
	}
}
#endif

OSStatus AU2_Wrapper::Reset(AudioUnitScope inScope, AudioUnitElement inElement)
{
#if DEBUG_PRINT
	printf("AU2_Wrapper::Reset\n");
#endif
	if (inScope == kAudioUnitScope_Global)
	{
		mAbsoluteSampleFrame = 0;

        // intended for situation where processor has been suspended/resumed and we don't want to hear the tail of old conent.
//        processor.ClearDelaysUnsafe();
	}
	return AUBase::Reset(inScope, inElement);
}

void AU2_Wrapper::PerformEvents(const AudioTimeStamp& inTimeStamp)
{
	std::lock_guard<std::mutex> guard(hostMidiLock);

	// switch buffers.
	const int readingMidiEvents = curMidiEvents.load();
	{
		curMidiEvents.store((readingMidiEvents + 1) & 1);
	}
#if 0
    
    int crashAfter{};
	while (!midiEvents[readingMidiEvents].IsEmpty())
	{
		auto e = midiEvents[readingMidiEvents].Current();

		midiConverter.processMidi({ e->data, e->size }, static_cast<int>(e->timestamp));

		midiEvents[readingMidiEvents].UpdateReadPos();
        
        crashAfter++;
	}
#endif
	for (auto& p : parameterChanges[readingMidiEvents])
	{
        // we no longer use 'kMustUpdateUi' otherwise it results in events being sent back to GUI, except late. (jitter).
        // The GUI is already notified via it's event listerner when the DAW changes a param
//		processor.setParameterNormalizedDsp(p.BufferOffsetInFrames, p.ID, p.Value, 0);
	}

	parameterChanges[readingMidiEvents].clear();
}

OSStatus AU2_Wrapper::SetParameter(
	AudioUnitParameterID		inID,
	AudioUnitScope 				inScope,
	AudioUnitElement 			inElement,
	AudioUnitParameterValue		inValue,
	UInt32						inBufferOffsetInFrames)
{
    if(inID < 0 || inID >= gmpiController.nativeParams.size())
        return kAudioUnitErr_InvalidParameter;
    
    auto p = plugin.nativeParams[inID];
    
    plugin.setParameterNormalizedFromDaw(
         *plugin.info
        , inBufferOffsetInFrames
        , p->info->id
        , plugin.nativeParams[inID]->real2Normalized(inValue)
        );
    
//	if (auto p = getDawParameter(inID); p)
	{
#if 0
		// communicate change to UI (GarageBand and Logic Pro don't seem to do this via the parameter listener (Ableton Live does))
		stateMgr.onParameterAutomation(inID, inValue);
#endif
//		p->setValueImmediate(inValue);

#ifdef _DEBUG
		std::cerr << "DAW => SetParameter(" << inID << ", " << inValue << ")" << std::endl;
#endif

//		_RPT2(_CRT_WARN, "            PRESETS-DSP: parameterChanges.push_back(P%d SPN(%f)\n", inID, inValue);
//		const auto daw_normalized = p->convertNormalized(p->RealToNormalized(inValue)); // normalized from the DAWs perspective (may be inverted)

		std::lock_guard<std::mutex> guard(hostMidiLock);
//		parameterChanges[curMidiEvents.load()].push_back({inBufferOffsetInFrames, inID, static_cast<AudioUnitParameterValue>(daw_normalized)});
	}
	return noErr;
}

OSStatus AU2_Wrapper::GetParameter(
    AudioUnitParameterID	inID,
	AudioUnitScope 			inScope,
	AudioUnitElement 		inElement,
	AudioUnitParameterValue& outValue)
{
	// !!! Ableton Live queries this right after SetParameter (which is not applied yet, due to queing)
	// need to set normalised value on parameter (that does not affect actual value).
	if (inScope == kAudioUnitScope_Global)
	{
        assert(inID >= 0 && inID < gmpiController.nativeParams.size());
        
        if(inID < 0 || inID >= gmpiController.nativeParams.size())
            return kAudioUnitErr_InvalidParameter;
        
        auto p = plugin.nativeParams[inID];

        outValue = p->valueReal();
        return noErr;
	}

	return kAudioUnitErr_InvalidScope;
}

OSStatus AU2_Wrapper::Render(AudioUnitRenderActionFlags& ioActionFlags,
	const AudioTimeStamp& inTimeStamp,
	UInt32 inNumberFrames)
{
	auto& plugin_ = plugin.processor;
	auto& events = plugin.events;

    queueToDsp_.pollMessage(&plugin);

#if 0
	if (processor.reinitializeFlag)
	{
		reInitialize();
	}
#endif

    {
        Float64    currentBeat = 0;
        Float64 currentTempo = 120;
        
        if (CallHostBeatAndTempo(&currentBeat, &currentTempo) == noErr)
        {
            if (!isfinite(currentBeat))
            {
                currentBeat= 0.0;
            }
        }
        
        UInt32 outDeltaSampleOffsetToNextBeat{};
        double outCurrentMeasureDownBeat{};
        float num{4.f};
        UInt32 den{4};

        if (CallHostMusicalTimeLocation(&outDeltaSampleOffsetToNextBeat, &num, &den,
            &outCurrentMeasureDownBeat) == noErr)
        {
//            timeInfo.barStartPos = outCurrentMeasureDownBeat;
        }
        
        plugin.setHostControlFromDaw(gmpi::hosting::HostControls::TimeBpm,                 currentTempo);
        plugin.setHostControlFromDaw(gmpi::hosting::HostControls::TimeNumerator,           num);
        plugin.setHostControlFromDaw(gmpi::hosting::HostControls::TimeDenominator,         den);
        plugin.setHostControlFromDaw(gmpi::hosting::HostControls::TimeQuarterNotePosition, currentBeat);       
    }
    
#if 0
	if (processor.NeedsTempo())
	{
		enum
		{
			kVstTransportChanged = 1,
			kVstTransportPlaying = 1 << 1,
			kVstTransportCycleActive = 1 << 2,

			kVstAutomationWriting = 1 << 6,
			kVstAutomationReading = 1 << 7,

			// flags which indicate which of the fields in this VstTimeInfo
			//  are valid; samplePos and sampleRate are always valid
			kVstNanosValid = 1 << 8,
			kVstPpqPosValid = 1 << 9,
			kVstTempoValid = 1 << 10,
			kVstBarsValid = 1 << 11,
			kVstCyclePosValid = 1 << 12,	// start and end
			kVstTimeSigValid = 1 << 13,
			kVstSmpteValid = 1 << 14,
			kVstClockValid = 1 << 15
		};

		timeInfo.flags = kVstTempoValid | kVstPpqPosValid | kVstBarsValid | kVstTimeSigValid;

		Float64	currentBeat = 0;
		Float64 currentTempo = 120;

		if (CallHostBeatAndTempo(&currentBeat, &currentTempo) == noErr)
		{
			timeInfo.tempo = currentTempo;
			timeInfo.ppqPos = currentBeat;

			// Fix for AU Validation which passes ppqPos = inf
#ifndef _MSC_VER
			if (!isfinite(timeInfo.ppqPos))
			{
				timeInfo.ppqPos = 0.0;
			}
#endif
		}
		else
		{
			timeInfo.tempo = 120;
			timeInfo.ppqPos = 0;
		}

		UInt32 outDeltaSampleOffsetToNextBeat;
		double outCurrentMeasureDownBeat;
		float num;
		UInt32 den;

		if (CallHostMusicalTimeLocation(&outDeltaSampleOffsetToNextBeat, &num, &den,
			&outCurrentMeasureDownBeat) == noErr)
		{
			timeInfo.timeSigNumerator = (int)num;
			timeInfo.timeSigDenominator = (int)den;
			timeInfo.barStartPos = outCurrentMeasureDownBeat;
		}
		else
		{
			timeInfo.timeSigNumerator = 4;
			timeInfo.timeSigDenominator = 4;
			timeInfo.barStartPos = 0;
		}

		Boolean playing, looping, transportStateChanged;
		Float64 outCycleStartBeat, outCycleEndBeat, outCurrentSampleInTimeLine;
		if (CallHostTransportState(&playing,
			&transportStateChanged,
			&outCurrentSampleInTimeLine,
			&looping,
			&outCycleStartBeat,
			&outCycleEndBeat) == noErr)
		{
			timeInfo.samplePos = outCurrentSampleInTimeLine;
			if (playing)
			{
				timeInfo.flags |= kVstTransportPlaying;
			}

			if (transportStateChanged)
			{
				timeInfo.flags |= kVstTransportChanged;
			}
		}

		processor.UpdateTempo(&timeInfo);
	}
#endif
    
	PerformEvents(inTimeStamp);

	int validInputChannels = 0;
	for (int busIdx = 0; busIdx < Inputs().GetNumberOfElements(); ++busIdx)
	{
		auto& io = Input(busIdx);
		if (io.IsActive())
		{
			io.PullInput(ioActionFlags, inTimeStamp, busIdx, inNumberFrames);
		}

		if (io.IsActive())
		{
			AudioBufferList& bufferList = io.GetBufferList();
			for (int i = 0; i < io.GetStreamFormat().mChannelsPerFrame /* .NumberChannels() */; ++i)
			{
				inputPtr[validInputChannels++] = (float*)bufferList.mBuffers[i].mData;
			}
		}
	}
	for (int i = validInputChannels; i < inputPtr.size(); ++i)
	{
		inputPtr[i] = dummyInputBuffer;
	}

	int validOutputChannels = 0;
	for (int busIdx = 0; busIdx < Outputs().GetNumberOfElements(); ++busIdx)
	{
		auto& io = Output(busIdx);
		io.PrepareBuffer(inNumberFrames);
		AudioBufferList& bufferList = io.GetBufferList();
		for (int i = 0; i < io.GetStreamFormat().mChannelsPerFrame /*.NumberChannels()*/; ++i)
		{
			outputPtr[validOutputChannels++] = (float*)bufferList.mBuffers[i].mData;
		}
	}
	for (int i = validOutputChannels; i < outputPtr.size(); ++i)
	{
		outputPtr[i] = dummyOutputBuffer;
	}

	int64_t allSilenceFlagsIn{};
	int64_t allSilenceFlagsOut{};
/*
	processor.process(
		inNumberFrames
		, (const float**)inputPtr.data()
		, outputPtr.data()
		, validInputChannels
		, validOutputChannels
		, allSilenceFlagsIn
		, allSilenceFlagsOut
		);
*/
	auto& info = *gmpi::hosting::factory::getInstance().getPluginInfo();

    // pass buffer pointer to plugin
    {
        int inIdx = 0;
        int outIdx = 0;
        for (auto& pin : info.dspPins)
        {
            if (pin.datatype != gmpi::PinDatatype::Audio)
                continue;

            if (pin.direction == gmpi::PinDirection::In)
            {
                plugin_->setBuffer(pin.id, inputPtr[inIdx++]);
            }
            else
            {
                plugin_->setBuffer(pin.id, outputPtr[outIdx++]);
            }
        }

        assert(inIdx == inputPtr.size());
        assert(outIdx == outputPtr.size());
    }
    
	plugin_->process(inNumberFrames, events.head());

	events.clear();
    
    if(plugin.pendingControllerQueueClients.ServiceWaitersIncremental(
          &gmpiController.message_que_dsp_to_ui
        , inNumberFrames
    ))
    {
        gmpiController.message_que_dsp_to_ui.Send();
    }

	mAbsoluteSampleFrame += inNumberFrames;

	return noErr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	AU2_Wrapper::ValidFormat
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
bool AU2_Wrapper::ValidFormat(AudioUnitScope					inScope,
	AudioUnitElement				inElement,
	const AudioStreamBasicDescription& inNewFormat)
{
    bool isValid{};
    
    switch(inScope)
    {
        case kAudioUnitScope_Input:
            isValid = inElement < busArrangement.size() && inNewFormat.mChannelsPerFrame <= busArrangement[inElement].inChannels;
            break;
            
        case kAudioUnitScope_Output:
            isValid = inElement < busArrangement.size() && inNewFormat.mChannelsPerFrame <= busArrangement[inElement].outChannels;
            break;
            
        default:
            isValid = AUBase::ValidFormat(inScope, inElement, inNewFormat);
    }
    
#ifdef _DEBUG
	std::cerr << "ValidFormat(" << (inScope == kAudioUnitScope_Input ? "in " : "out") << ", " << inNewFormat.mChannelsPerFrame << "-chan) => " << (isValid ? "yes\n" : "no\n");
#endif
    
    return isValid;
}

UInt32 AU2_Wrapper::SupportedNumChannels(const AUChannelInfo** outInfo)
{
	/* seems fine, returns e.g. [0, 18] note AuSampler returns [0, -18] to indicate any number of outputs upto 18

	#ifdef _DEBUG
		std::cerr << "AU2_Wrapper::SupportedNumChannels() -> ";
		for(const auto& config : supportedChannels)
		{
			std::cerr << "[" << config.inChannels << ", " << config.outChannels << "] ";
		}
		std::cerr << std::endl;
	#endif
		*/
	if (outInfo)
		*outInfo = supportedChannels.data();

	return static_cast<UInt32>(supportedChannels.size());
}

bool AU2_Wrapper::StreamFormatWritable(AudioUnitScope					scope,
	AudioUnitElement				element)
{
	return IsInitialized() ? false : true;
}

#if 0
OSStatus AU2_Wrapper::HandleMidiEvent(UInt8 status, UInt8 channel, UInt8 data1, UInt8 data2, UInt32 inStartFrame)
{
	if (!wantsMidi)
	{
		return 1; // satisfy auval
	}

	const unsigned char data[] = { (unsigned char)(status | channel),
		(unsigned char)data1,
		(unsigned char)data2 };

	std::lock_guard<std::mutex> guard(hostMidiLock);
	midiEvents[curMidiEvents.load()].Add(inStartFrame, data, 3);

	return noErr;
}
#endif

#if 0
OSStatus AU2_Wrapper::MIDIEvent(
    UInt32 inStatus, UInt32 inData1, UInt32 inData2, UInt32 inOffsetSampleFrame)
{
    const UInt32 strippedStatus = inStatus & 0xf0U; // NOLINT
    const UInt32 channel = inStatus & 0x0fU;        // NOLINT

//    return HandleMIDIEvent(strippedStatus, channel, inData1, inData2, inOffsetSampleFrame);
    
    if (!wantsMidi)
	{
		return 1; // satisfy auval
	}

	const unsigned char data[] = { (unsigned char)(strippedStatus | channel),
		(unsigned char)inData1,
		(unsigned char)inData2 };

	std::lock_guard<std::mutex> guard(hostMidiLock);
//	midiEvents[curMidiEvents.load()].Add(inOffsetSampleFrame, data, 3);

	return noErr;
}
#endif

#if AUSDK_HAVE_MIDI2
OSStatus AU2_Wrapper::MIDIEventList(
    UInt32 inOffsetSampleFrame, const struct MIDIEventList* eventList)
{
    if (!wantsMidi)
	{
		return 1; // satisfy auval
	}
    
    static const int midi_message_size[16] = // in 32-bit words
    {
        1,
        1,
        1,
        2,
        2,
        4,
        1,
        1,
        2,
        2,
        2,
        3,
        3,
        4,
        4,
        4
    };
    
    uint8_t reversebuffer[8];

    const auto* packet = eventList->packet;
    for (unsigned i = 0; i < eventList->numPackets; ++i)
    {
        int j = 0;
        while(j < packet->wordCount)
        {
            const auto src = reinterpret_cast<const uint8_t*>(packet->words + j);
            
            const auto message_type = src[3] >> 4;
            const auto message_length = midi_message_size[message_type];
            
            if(message_length < 3)
            {
                int bufferIndex = 0;
                // 32-bit messages.
                reversebuffer[bufferIndex++] = src[3];
                reversebuffer[bufferIndex++] = src[2];
                reversebuffer[bufferIndex++] = src[1];
                reversebuffer[bufferIndex++] = src[0];
                
                if(message_length == 2)
                {
                    // 64-bit messages
                    reversebuffer[bufferIndex++] = src[7];
                    reversebuffer[bufferIndex++] = src[6];
                    reversebuffer[bufferIndex++] = src[5];
                    reversebuffer[bufferIndex++] = src[4];
                }
                
                midiConverter.processMidi(
                      { reversebuffer, static_cast<size_t>(message_length * 4) }
                      , static_cast<int>(inOffsetSampleFrame + packet->timeStamp)
                      );
            }
            
            j += message_length;
        }

        packet = MIDIEventPacketNext(packet);
    }
    
/*
	std::lock_guard<std::mutex> guard(hostMidiLock);
    
	const int current = curMidiEvents.load();

 	const auto* packet = eventList->packet;
	for (unsigned i = 0; i < eventList->numPackets; ++i)
    {
        // need to reverse endianess. same on M1?
        int bufferIndex = 0;
        for(int j = 0 ; j < packet->wordCount && bufferIndex < sizeof(midi2conversionbuffer); ++j)
        {
            const auto src = reinterpret_cast<const unsigned char*>(packet->words + j);
            midi2conversionbuffer[bufferIndex++] = src[3];
            midi2conversionbuffer[bufferIndex++] = src[2];
            midi2conversionbuffer[bufferIndex++] = src[1];
            midi2conversionbuffer[bufferIndex++] = src[0];
        }
        
		midiEvents[current].Add(
			inOffsetSampleFrame + packet->timeStamp,
            midi2conversionbuffer,
            bufferIndex
            );

		packet = MIDIEventPacketNext(packet);
	}
 */
    return noErr;
}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Filter::GetPropertyInfo
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
OSStatus	AU2_Wrapper::GetPropertyInfo(AudioUnitPropertyID		inID,
	AudioUnitScope					inScope,
	AudioUnitElement				inElement,
	UInt32& outDataSize,
	bool& outWritable)
{
	if (inScope == kAudioUnitScope_Global)
	{
		switch (inID)
		{
		case kAudioUnitProperty_CocoaUI:
			outWritable = false;
			outDataSize = sizeof(AudioUnitCocoaViewInfo);
			return noErr;

		case kAudioUnitProperty_PresentPreset:
		{
			outDataSize = sizeof(AUPreset);
		}

		case kAudioUnitProperty_FactoryPresets:
			//				ca_require(inScope == kAudioUnitScope_Global, InvalidScope);
			//				result = GetPresets(NULL);
			//				if (!result) {
			outDataSize = sizeof(CFArrayRef);
			outWritable = false;
			//				}
			break;

		case kAudioUnitProperty_ParameterStringFromValue:
			outDataSize = sizeof(AudioUnitParameterStringFromValue);
			outWritable = false;
			return noErr;

		case kAudioUnitProperty_ParameterValueFromString:
			outDataSize = sizeof(AudioUnitParameterValueFromString);
			outWritable = false;
			return noErr;

		case kAudioUnitProperty_OfflineRender:
			outWritable = true;
			outDataSize = sizeof(UInt32);
			return noErr;

		case kAudioUnitProperty_SupportsMPE:
			outDataSize = sizeof(UInt32);
			outWritable = false;
			return noErr;
            
        case kAudioUnitProperty_AudioUnitMIDIProtocol:
            outDataSize = sizeof (UInt32);
            outWritable = false;
            return noErr;

		case 64000:
		{
			if (true) //editController)
			{
				outDataSize = sizeof(void*);
				outWritable = false;
				return noErr;
			}
			return kAudioUnitErr_InvalidProperty;
		}
		}
	}

	return AUBase::GetPropertyInfo(inID, inScope, inElement, outDataSize, outWritable);
}

int heyLinkerDontDiscardAudioUnitView_mm();

CFBundleRef CreatePluginBundleRef()
{
    CFBundleRef rBundleRef = 0;
    
    Dl_info info;
    if (dladdr ((const void*)CreatePluginBundleRef, &info))
    {
        if (info.dli_fname)
        {
            std::string name;
            name.assign (info.dli_fname);
            for (int i = 0; i < 3; i++)
            {
                auto p = name.find_last_of ('/');
                if (p == std::string::npos)
                {
                    fprintf (stdout, "Could not determine bundle location.\n");
                    return 0; // unexpected
                }
                //                name.remove (delPos, name.length () - delPos);
                name = name.substr(0, p);
                
            }
            CFURLRef bundleUrl = CFURLCreateFromFileSystemRepresentation (0, (const UInt8*)name.c_str(), name.length (), true);
            if (bundleUrl)
            {
                rBundleRef = CFBundleCreate (0, bundleUrl);
                CFRelease (bundleUrl);
            }
        }
    }
    
    return rBundleRef;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Filter::GetProperty
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
OSStatus AU2_Wrapper::GetProperty(
    AudioUnitPropertyID 		inID,
	AudioUnitScope 				inScope,
	AudioUnitElement			inElement,
	void* outData)
{
	if (inScope == kAudioUnitScope_Global)
	{
		switch (inID)
		{
		case kAudioUnitProperty_CocoaUI:
		{
            static AudioUnitCocoaViewInfo info;
            
            heyLinkerDontDiscardAudioUnitView_mm();
            
            CFBundleRef bundle = CreatePluginBundleRef();

            if (bundle == NULL) return 1;

            CFURLRef url = CFBundleCopyBundleURL(bundle);
            CFRetain(url);

            CFStringRef className = CFStringCreateWithCString(NULL, "GMPI_VIEW_MAKER_VERSION_02", kCFStringEncodingUTF8);
            info = { url, {className} };

            CFRelease(bundle);
            
            *((AudioUnitCocoaViewInfo*)outData) = info;

			return noErr;
		}

		case kAudioUnitProperty_PresentPreset:
		{
			static AUPreset test;
			test.presetNumber = 0;
			test.presetName = CFSTR("Duck");

			*((AUPreset*)outData) = test;
			return noErr;
		}

		case kAudioUnitProperty_FactoryPresets:
		{
			//				clearPresetsArray();
			//				presetsArray.insertMultiple(0, AUPreset(), numPrograms);

			int numPrograms = 1;
			CFMutableArrayRef presetsArrayRef = CFArrayCreateMutable(0, numPrograms, 0);

			static AUPreset test;
			test.presetNumber = 0;
			test.presetName = CFSTR("Duck");
			CFArrayAppendValue(presetsArrayRef, &test);

			*((CFMutableArrayRef*)outData) = presetsArrayRef;
			/*
			for (int i = 0; i < numPrograms; ++i)
			{
				String name(juceFilter->getProgramName(i));
				if (name.isEmpty())
					name = "Untitled";

				AUPreset& p = presetsArray.getReference(i);
				p.presetNumber = i;
				p.presetName = name.toCFString();

				CFArrayAppendValue(presetsArrayRef, &p);
			}

			*(CFArrayRef *)outData = NULL;
			result = GetPresets((CFArrayRef *)outData);
*/
		}
		break;

		case kAudioUnitProperty_ParameterValueFromString:
		{
			if (inScope != kAudioUnitScope_Global)
				return kAudioUnitErr_InvalidScope;

			if (auto vfs = (AudioUnitParameterValueFromString*)outData)
			{
                if(vfs->inParamID < 0 || vfs->inParamID >= gmpiController.nativeParams.size())
                    return kAudioUnitErr_InvalidParameter;
                
                auto p = gmpiController.nativeParams[vfs->inParamID];

				const CFIndex bufferSize = CFStringGetLength(vfs->inString) + 1; // The +1 is for NUL terminated
				char buffer[bufferSize];
				if (!CFStringGetCString(vfs->inString, buffer, bufferSize, kCFStringEncodingUTF8))
				{
					return kAudioUnitErr_InvalidParameter;
				}

                const std::string text(buffer);
                
                for(auto&[id, name] : p->info->enum_entries)
                {
                    if (name == text)
                    {
                        vfs->outValue = id;
                        return noErr;
                    }
                }
 /*
                const auto enum_list = wrapper::Utf8ToWstring(p->info->enum_list);

				it_enum _list it(enum_list);
				for (it.First(); !it.IsDone(); ++it)
				{
					if (it.CurrentItem()->text == text)
					{
						vfs->outValue = it.CurrentItem()->index; //value;
						return noErr;
					}
				}
  */
				return kAudioUnitErr_InvalidParameter;
			}

			return AUBase::GetProperty(inID, inScope, inElement, outData);
		}
        break;
                
		case kAudioUnitProperty_ParameterStringFromValue:
		{
			if (inScope != kAudioUnitScope_Global)
				return kAudioUnitErr_InvalidScope;

			if (auto pv = (AudioUnitParameterStringFromValue*)outData)
			{
                if(pv->inParamID < 0 || pv->inParamID >= gmpiController.nativeParams.size())
                    return kAudioUnitErr_InvalidParameter;
                
                auto p = gmpiController.nativeParams[pv->inParamID];

                const auto nearest = static_cast<int>(std::round(*pv->inValue));
                for(auto&[id, name] : p->info->enum_entries)
                {
                    if (id == nearest)
                    {
                        pv->outString = CFStringCreateWithCString(
                            kCFAllocatorDefault,
                            name.c_str(),
                            CFStringGetSystemEncoding()
                        );
                        return noErr;
                    }
                }
/*
                const auto enum_list = wrapper::Utf8ToWstring(p->info->enum_list);

				it_ enum_list it(enum_list);
				it.FindIndex(static_cast<int>(0.5f + *pv->inValue));
				if (!it.IsDone())
				{
                    auto text = wrapper::WStringToUtf8(it.CurrentItem()->text);

					pv->outString = CFStringCreateWithCString(
						kCFAllocatorDefault,
						text.c_str(),
						CFStringGetSystemEncoding()
					);
					return noErr;
				}
 */
				return kAudioUnitErr_InvalidParameter;
			}

			return noErr;
		}
        break;
                
		case kAudioUnitProperty_OfflineRender:
			*(UInt32*)outData = offLineRenderMode;
			return noErr;

		case kAudioUnitProperty_SupportsMPE:
			*(UInt32*)outData = 1;
			return noErr;

        case kAudioUnitProperty_AudioUnitMIDIProtocol:
			*(UInt32*) outData = kMIDIProtocol_2_0;
			return noErr;

		case 64000: // get EditorHost
		{
			if (true)//VST3DynLibrary::gInstance)
			{
				void* ptr = static_cast<gmpi::api::IEditorHost*>(&gmpiController);
				*((void**)outData) = ptr;
				return noErr;
			}
			else
				*((void**)outData) = 0;
			return kAudioUnitErr_InvalidProperty;
		}
		/*
			// This is our custom property which reports the current frequency response curve
			//
			case kAudioUnitCustomProperty_FilterFrequencyResponse:
			{
			if(inScope != kAudioUnitScope_Global) 	return kAudioUnitErr_InvalidScope;

			// the kernels are only created if we are initialized
			// since we're using the kernels to get the curve info, let
			// the caller know we can't do it if we're un-initialized
			// the UI should check for the error and not draw the curve in this case
			if(!IsInitialized() ) return kAudioUnitErr_Uninitialized;

			FrequencyResponse *freqResponseTable = ((FrequencyResponse*)outData);

			// each of our filter kernel objects (one per channel) will have an identical frequency response
			// so we arbitrarilly use the first one...
			//
			FilterKernel *filterKernel = dynamic_cast<FilterKernel*>(mKernelList[0]);


			double cutoff = GetParameter(kFilterParam_CutoffFrequency);
			double resonance = GetParameter(kFilterParam_Resonance );

			float srate = GetSampleRate();

			cutoff = 2.0 * cutoff / srate;
			if(cutoff > 0.99) cutoff = 0.99;		// clip cutoff to highest allowed by sample rate...

			filterKernel->CalculateLopassParams(cutoff, resonance);

			for(int i = 0; i < kNumberOfResponseFrequencies; i++ )
			{
			double frequency = freqResponseTable[i].mFrequency;

			freqResponseTable[i].mMagnitude = filterKernel->GetFrequencyResponse(frequency);
			}

			return noErr;
			}
			*/
		}
	}

	// if we've gotten this far, handles the standard properties
	return AUBase::GetProperty(inID, inScope, inElement, outData);
}

OSStatus AU2_Wrapper::SetProperty(AudioUnitPropertyID             inID,
	AudioUnitScope                  inScope,
	AudioUnitElement                inElement,
	const void* inData,
	UInt32                          inDataSize)
{
	if (inScope == kAudioUnitScope_Global)
	{
		switch (inID)
		{
		case kAudioUnitProperty_OfflineRender:
			offLineRenderMode = *(UInt32*)inData;
			reInitialize();
			return noErr;
		}
	}

	return AUBase::SetProperty(inID, inScope, inElement, inData, inDataSize);
}

OSStatus AU2_Wrapper::GetParameterInfo(
    AudioUnitScope					inScope,
	AudioUnitParameterID			inParameterID,
	AudioUnitParameterInfo& outParameterInfo)
{
	if (inScope != kAudioUnitScope_Global)
		return kAudioUnitErr_InvalidScope;

	if (inParameterID < 0 || inParameterID >= static_cast<int>(gmpiController.nativeParams.size()))
		return kAudioUnitErr_InvalidParameter;

	const auto& p = *gmpiController.nativeParams[inParameterID];

	outParameterInfo.name[0] = 0;
	outParameterInfo.flags =
		kAudioUnitParameterFlag_IsWritable
		| kAudioUnitParameterFlag_IsReadable
		| kAudioUnitParameterFlag_IsHighResolution
		| kAudioUnitParameterFlag_HasCFNameString
		| kAudioUnitParameterFlag_CFNameRelease;

	if (!p.info->enum_entries.empty())
	{
		outParameterInfo.flags |= kAudioUnitParameterFlag_ValuesHaveStrings;
		outParameterInfo.unit = kAudioUnitParameterUnit_Indexed;

		// Mac enum params have to have consecutive values.
		outParameterInfo.minValue = 0;
		outParameterInfo.maxValue = (std::max)(0, static_cast<int>(p.info->enum_entries.size()) - 1);
	}
	else
	{
        outParameterInfo.minValue = p.info->minimum;
        outParameterInfo.maxValue = p.info->maximum;

		outParameterInfo.flags |= kAudioUnitParameterFlag_CanRamp;
		outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
	}

    outParameterInfo.defaultValue = atof(p.info->default_value_s.c_str());// p.info->default_value;

	outParameterInfo.clumpID = 0;

    const auto& name_utf8 = p.info->name;
	strlcpy(outParameterInfo.name, name_utf8.c_str(), sizeof(outParameterInfo.name));

	outParameterInfo.cfNameString = CFStringCreateWithCString(NULL, name_utf8.c_str(), kCFStringEncodingUTF8);

	return noErr;
}

OSStatus AU2_Wrapper::GetParameterValueStrings(
    AudioUnitScope          inScope,
	AudioUnitParameterID    inParameterID,
	CFArrayRef* outStrings)
{
	if (inScope != kAudioUnitScope_Global)
		return kAudioUnitErr_InvalidScope;

	if (inParameterID < 0 || inParameterID >= static_cast<int>(gmpiController.nativeParams.size()))
		return kAudioUnitErr_InvalidParameter;

	const auto& p = *gmpiController.nativeParams[inParameterID];

	if (p.info->enum_entries.empty())
		return kAudioUnitErr_InvalidParameter;

	if (!outStrings)
	{
		return noErr;
	}

    const auto& enumList = p.info->enum_list;

	std::vector<CFStringRef>* strings = {};
	auto it = enumStrings.find(enumList);
	if (it != enumStrings.end())
	{
		strings = &(*it).second;
	}
	else
	{
		strings = &(enumStrings[enumList]);

        for(auto&[id, name] : p.info->enum_entries)
        {
            strings->push_back(
                CFStringCreateWithCString(
                    kCFAllocatorDefault,
                    name.c_str(),
                    CFStringGetSystemEncoding()
                )
            );
        }
        /*
		it_enum_list it(enumList);
		for (it.First(); !it.IsDone(); ++it)
		{
            const auto text = wrapper::WStringToUtf8(it.CurrentItem()->text);

			strings->push_back(
				CFStringCreateWithCString(
					kCFAllocatorDefault,
					text.c_str(),
					CFStringGetSystemEncoding()
				)
			);
		}
         */
	}

	*outStrings = CFArrayCreate(
		nullptr,
		(const void**)strings->data(),
		strings->size(),
		nullptr
	);

	return noErr;
}

OSStatus AU2_Wrapper::SaveState(CFPropertyListRef* outData)
{
	auto result = AUBase::SaveState(outData);

	if (result == noErr)
	{
		auto dict = (CFMutableDictionaryRef)*outData;

// seems not (sawdemo)        assert(std::this_thread::get_id() == mainThreadID );
//        const auto chunk = getPreset()->toString(wrapper::BundleInfo::instance()->getPluginId());

//		std::string chunk;
//		processor.getPresetState(chunk, true);
		const auto chunk = gmpiController.getPreset();

		CFStringRef s = CFStringCreateWithCString(NULL, chunk.c_str(), kCFStringEncodingUTF8);
		CFDictionaryAddValue(dict, CFSTR("GMPIPRESET"), s);

		CFRelease(s);
	}

	return result;
}

void AU2_Wrapper::flushPendingParameterUpdates()
{
	std::lock_guard<std::mutex> guard(hostMidiLock);

	const auto current = curMidiEvents.load();

	// When Processor is suspended by DAW, Parameter changes can build up in que without being applied to patchmanager.
	// If the DAW then asks for plugins state, we need to apply these changes first, elase XML will be out-of-date.
	const int timestamp = 0;
	for (auto& p : parameterChanges[current])
	{
//		processor.setParameterNormalizedDsp(timestamp, p.ID, p.Value, 0);
	}

	parameterChanges[current].clear();
}

OSStatus AU2_Wrapper::RestoreState(CFPropertyListRef plist)
{
	//   std::cout << "AU2_Wrapper::RestoreState() - START" << std::endl;
	auto result = AUBase::RestoreState(plist);

	if (result == noErr)
	{
		auto dict = (CFMutableDictionaryRef)plist;

		auto s = reinterpret_cast<CFStringRef>(CFDictionaryGetValue(dict, CFSTR("GMPIPRESET")));
		if (s == NULL)
			return kAudioUnitErr_InvalidPropertyValue;

		CFIndex size = CFStringGetLength(s);
		CFIndex maxSize = CFStringGetMaximumSizeForEncoding(size, kCFStringEncodingUTF8) + 1;

		std::string temp;
		temp.resize(maxSize);

		CFIndex usedBytes = 0L;
		CFStringGetBytes(s, CFRangeMake(0L, size), kCFStringEncodingUTF8, '?', false, (UInt8*)temp.data(), temp.size(), &usedBytes);

		std::string chunk(temp.c_str()); // throws away padding zeros at end.

		if (chunk.empty())
		{
			return noErr;
		}

// moved		setPresetFromDaw(chunk, false);
//        stateMgr.setPresetFromXml(chunk);

		gmpiController.setPresetXmlFromDaw(chunk);
	}
	//    std::cout << "AU2_Wrapper::RestoreState() - END" << std::endl;

	return result;
}

void AU2_Wrapper::setPresetXmlFromSelf(const std::string& xml)
{
//	stateMgr.setPresetFromXml(xml);
}

    /*
void AU2_Wrapper::setPresetFromSelf(DawPreset const* preset)
{
//    stateMgr.setPresetFromUnownedPtr(preset);
}
*/
    
void AU2_Wrapper::saveNativePreset(const char* filename, const std::string& presetName, const std::string& xml)
{
#if 0 // TODO
    const auto p_id = wrapper::BundleInfo::instance()->getPluginId();

	char i[5];
	i[3] = p_id & 0xff;
	i[2] = (p_id >> 8) & 0xff;
	i[1] = (p_id >> 16) & 0xff;
	i[0] = (p_id >> 24) & 0xff;
	i[4] = 0;

	const std::string pluginId(i);
/*
	AuPresetUtil::WritePreset(
		Utf8ToWstring(filename),
		presetName,
		pluginType,
		manufacturerId,
		pluginId,
		xml
	);
 */
#endif
}

std::string AU2_Wrapper::loadNativePreset(std::wstring sourceFilename)
{
    return {};//AuPresetUtil::ReadPreset(sourceFilename);
}

// IAudioPluginHost
gmpi::ReturnCode AU2_Wrapper::setPin(int32_t timestamp, int32_t pinId, int32_t size, const uint8_t* data)
{
    return plugin.setPin(timestamp, pinId, size, data);
}

gmpi::ReturnCode AU2_Wrapper::setPinStreaming(int32_t timestamp, int32_t pinId, bool isStreaming)
{
    return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode AU2_Wrapper::setLatency(int32_t latency)
{
    return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode AU2_Wrapper::sleep()
{
    return gmpi::ReturnCode::Ok;
}

int32_t AU2_Wrapper::getBlockSize()
{
    return GetMaxFramesPerSlice();
}

float AU2_Wrapper::getSampleRate()
{
    return sampleRate;
}

int32_t AU2_Wrapper::getHandle()
{
    return 0; // only one plugin, can have handle zero.
}
