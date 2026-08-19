#import "AU3_Wrapper.h"
#import <AVFAudio/AVFAudio.h>
#import <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include "GmpiSdkCommon.h"
#include "GmpiMidi.h"
#include "Hosting/xml_spec_reader.h"
#include "Hosting/gmpi_factory.h"
#include "Hosting/processor_holder.h"
#include "Hosting/controller_holder.h"
#include "Hosting/message_queues.h"
#include "helpers/Timer.h"

// provide extensibility to add extra modules on a per-project basis.
// Defined in AU2_Wrapper.cpp when both wrappers are linked; weak here so a
// module linking only AU3 still resolves it.
__attribute__((weak)) void initialise_synthedit_extra_modules(bool passFalse)
{
	// here to satisfy linker
}

namespace {

// The all-C++ heart of the wrapper. The Objective-C class below is a thin
// shell around one of these so the render block captures a plain pointer and
// the real-time path never dispatches through Objective-C.
//
// Thread domains, same split as the AU2 wrapper:
//   * `plugin` (processor store + event queue) belongs to the RENDER thread.
//   * `gmpiController` (controller store + editors) belongs to the MAIN thread.
//   * queueToDsp carries main -> render; message_que_dsp_to_ui carries
//     render -> main, drained by the 15ms timer.
// Unlike AU2 - whose SetParameter writes the render-side event list from the
// host's thread - every main-thread parameter change here rides queueToDsp, so
// the queues are the only crossing points.
struct AU3Core : public gmpi::api::IProcessorHost, public gmpi::TimerClient
{
	static constexpr int timerPeriodMs = 15; // ~60Hz, matching AU2 and the drawing frame's tick.

	gmpi::hosting::gmpi_processor plugin;
	gmpi::hosting::gmpi_controller_holder gmpiController;
	gmpi::hosting::interThreadQue queueToDsp{0x500000}; // 5MB, see AU2 AUDIO_MESSAGE_QUE_SIZE
	std::unique_ptr<gmpi::midi::MidiConverter2> midiConverter;

	// Fixed channel layout, discovered once from the plugin's pin list.
	int inputCount{};
	int outputCount{};
	std::vector<float*> inputPtr;
	std::vector<float*> outputPtr;

	// Owned sample storage: input pull destination, output fallback for hosts
	// that pass null mData, and one silent page for surplus plugin pins.
	std::vector<std::vector<float>> inputStorage;
	std::vector<std::vector<float>> outputStorage;
	std::vector<float> dummyInputBuffer;
	std::vector<float> dummyOutputBuffer;

	// AudioBufferList scratch for pulling input - sized and laid out in
	// allocateRenderResources so the render thread only rewrites pointers.
	std::vector<uint8_t> inputABLStorage;

	// Host context, captured at allocateRenderResources. ObjC blocks held from
	// C++; copied/released manually because this library builds without ARC.
	AUHostMusicalContextBlock musicalContextBlock{};

	float sampleRate{44100.0f};
	int32_t maxFrames{512};
	bool processorIsInitialized{};

	// setFullState mirrors preset values onto the AUParameterTree purely for
	// the host's benefit; this stops the implementor observer treating each
	// mirror-write as a fresh edit.
	std::atomic<bool> suppressParameterObserver{};

	AU3Core()
	{
		midiConverter = std::make_unique<gmpi::midi::MidiConverter2>(
			[this](const gmpi::midi2::message_view& msg, int sampleOffset)
			{
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
			});

		auto& info = *gmpi::hosting::factory::getInstance().getPluginInfo();
		plugin.init(info);
		gmpiController.init(info);

		inputCount  = countPins(info, gmpi::PinDirection::In,  gmpi::PinDatatype::Audio);
		outputCount = countPins(info, gmpi::PinDirection::Out, gmpi::PinDatatype::Audio);
		inputPtr.assign(inputCount, nullptr);
		outputPtr.assign(outputCount, nullptr);

		// Blob/string parameters can't ride the scalar parameter path; the
		// holder hands them over already framed for the processor's queue
		// ("ppc3", see GmpiParameter::getQueMessage).
		gmpiController.sendNonNativeParameterToProcessor = [this](gmpi::hosting::GmpiParameter* param)
		{
			sendParameterToProcessorQueue(param);
		};
	}

	~AU3Core()
	{
		stopTimer();
	}

	// Frame one parameter ("ppc2" scalar / "ppc3" blob) into the ui->dsp
	// queue. Main thread only - the queue is single-producer.
	void sendParameterToProcessorQueue(gmpi::hosting::GmpiParameter* param)
	{
		const auto messageLength = param->queryQueMessageLength(0);
		gmpi::hosting::my_msg_que_output_stream strm(&queueToDsp); // no header args: getQueMessage writes the whole frame.
		param->getQueMessage(strm, messageLength);
		strm.Send();
	}

	// One entry point for every host-side parameter change (automation, host
	// UI, our own editor via notifyDaw -> tree -> here). Main thread.
	void onParameterFromHost(AUParameterAddress address, AUValue value)
	{
		if (suppressParameterObserver.load(std::memory_order_relaxed))
			return;

		if (address >= gmpiController.nativeParams.size())
			return;

		auto p = gmpiController.nativeParams[address];

		// Change-detected: when our own editor originated this (setPinFromUi
		// already stored it), setParameterReal answers null and the GUI is not
		// echoed - the same jitter AU2 avoids with its listener-exclusion dance.
		if (auto changed = gmpiController.patchManager.setParameterReal(p->info->id, value); changed)
			gmpiController.notifyGui(changed);

		// Always forward: the processor's own store change-detects, so a
		// duplicate costs a few queue bytes, never a wrong value.
		sendParameterToProcessorQueue(p);
	}

	// gmpi::TimerClient - main thread, parameter updates from the processor.
	bool onTimer() override
	{
		gmpiController.message_que_dsp_to_ui.pollMessage(&gmpiController);
		return true;
	}

	// IProcessorHost (render thread; identical to AU2's forwarding)
	gmpi::ReturnCode setPin(int32_t timestamp, int32_t pinId, int32_t size, const uint8_t* data) override
	{
		return plugin.setPin(timestamp, pinId, size, data);
	}
	gmpi::ReturnCode setPinStreaming(int32_t timestamp, int32_t pinId, bool isStreaming) override
	{
		return gmpi::ReturnCode::Ok;
	}
	gmpi::ReturnCode setLatency(int32_t latency) override
	{
		return gmpi::ReturnCode::Ok;
	}
	gmpi::ReturnCode sleep() override
	{
		return gmpi::ReturnCode::Ok;
	}
	int32_t getBlockSize() override
	{
		return maxFrames;
	}
	float getSampleRate() override
	{
		return sampleRate;
	}
	int32_t getHandle() override
	{
		return 0; // only one plugin, can have handle zero.
	}

	gmpi::ReturnCode queryInterface(const gmpi::api::Guid* iid, void** returnInterface) override
	{
		*returnInterface = {};
		if ((*iid) == gmpi::api::IProcessorHost::guid || (*iid) == gmpi::api::IUnknown::guid)
		{
			*returnInterface = static_cast<gmpi::api::IProcessorHost*>(this);
			addRef();
			return gmpi::ReturnCode::Ok;
		}
		return gmpi::ReturnCode::NoSupport;
	}
	GMPI_REFCOUNT_NO_DELETE;
};

// UMP words arrive big-endian-word-packed from CoreMIDI; GMPI wants a byte
// stream. Same per-message-type word counts and byte reversal as
// AU2_Wrapper::MIDIEventList.
constexpr int midi_message_size[16] = // in 32-bit words
{
	1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4
};

void processUmpWords(AU3Core& core, const uint32_t* words, uint32_t wordCount, int sampleOffset)
{
	uint8_t reversebuffer[8];

	uint32_t j = 0;
	while (j < wordCount)
	{
		const auto src = reinterpret_cast<const uint8_t*>(words + j);

		const auto message_type = src[3] >> 4;
		const auto message_length = midi_message_size[message_type];

		if (message_length < 3) // 32 and 64-bit messages only, like AU2.
		{
			int bufferIndex = 0;
			reversebuffer[bufferIndex++] = src[3];
			reversebuffer[bufferIndex++] = src[2];
			reversebuffer[bufferIndex++] = src[1];
			reversebuffer[bufferIndex++] = src[0];

			if (message_length == 2)
			{
				reversebuffer[bufferIndex++] = src[7];
				reversebuffer[bufferIndex++] = src[6];
				reversebuffer[bufferIndex++] = src[5];
				reversebuffer[bufferIndex++] = src[4];
			}

			core.midiConverter->processMidi(
				{ reversebuffer, static_cast<size_t>(message_length * 4) }
				, sampleOffset
			);
		}

		j += message_length;
	}
}

} // namespace

@implementation GmpiAudioUnit
{
	std::unique_ptr<AU3Core> core;

	AUAudioUnitBusArray* _inputBusArray;
	AUAudioUnitBusArray* _outputBusArray;
	AUParameterTree* _parameterTree;
}

- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
									 options:(AudioComponentInstantiationOptions)options
									   error:(NSError**)outError
{
	self = [super initWithComponentDescription:componentDescription options:options error:outError];
	if (!self)
		return nil;

	core = std::make_unique<AU3Core>();

	auto& info = *gmpi::hosting::factory::getInstance().getPluginInfo();

	// Busses: one input bus (when the plugin has audio inputs) and one output
	// bus carrying every channel. The AU2 wrapper splits stereo pairs across
	// busses for Logic's multi-out routing; that refinement can come later
	// without changing this class's shape.
	NSMutableArray<AUAudioUnitBus*>* inputBusses = [NSMutableArray new];
	NSMutableArray<AUAudioUnitBus*>* outputBusses = [NSMutableArray new];

	if (core->inputCount > 0)
	{
		AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
																			   channels:(AVAudioChannelCount)core->inputCount];
		AUAudioUnitBus* bus = [[AUAudioUnitBus alloc] initWithFormat:format error:nil];
		bus.supportedChannelCounts = @[@(core->inputCount)];
		[inputBusses addObject:bus];
		[bus release];
		[format release];
	}
	if (core->outputCount > 0)
	{
		AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
																			   channels:(AVAudioChannelCount)core->outputCount];
		AUAudioUnitBus* bus = [[AUAudioUnitBus alloc] initWithFormat:format error:nil];
		bus.supportedChannelCounts = @[@(core->outputCount)];
		[outputBusses addObject:bus];
		[bus release];
		[format release];
	}

	_inputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
															busType:AUAudioUnitBusTypeInput
															 busses:inputBusses];
	_outputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
															 busType:AUAudioUnitBusTypeOutput
															  busses:outputBusses];
	[inputBusses release];
	[outputBusses release];

	// Parameter tree: one AUParameter per native parameter; the address IS the
	// dawTag, which init() guarantees equals the index into nativeParams.
	NSMutableArray<AUParameter*>* params = [NSMutableArray new];
	for (auto p : core->gmpiController.nativeParams)
	{
		const auto address = static_cast<AUParameterAddress>(p->info->dawTag);
		NSString* identifier = [NSString stringWithFormat:@"param%d", p->info->dawTag];
		NSString* name = [NSString stringWithUTF8String:p->info->name.c_str()];
		if (!name)
			name = identifier;

		AUParameter* param{};
		if (!p->info->enum_entries.empty())
		{
			NSMutableArray<NSString*>* strings = [NSMutableArray new];
			for (auto& [id, entryName] : p->info->enum_entries)
			{
				NSString* s = [NSString stringWithUTF8String:entryName.c_str()];
				[strings addObject:(s ? s : @"?")];
			}

			// Consecutive values 0..n-1, exactly as AU2's GetParameterInfo
			// flattens enums for the Mac.
			param = [AUParameterTree createParameterWithIdentifier:identifier
															  name:name
														   address:address
															   min:0.0f
															   max:(AUValue)std::max<size_t>(1, p->info->enum_entries.size()) - 1
															  unit:kAudioUnitParameterUnit_Indexed
														  unitName:nil
															 flags:kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_IsReadable
													  valueStrings:strings
											   dependentParameters:nil];
			[strings release];
		}
		else
		{
			param = [AUParameterTree createParameterWithIdentifier:identifier
															  name:name
														   address:address
															   min:(AUValue)p->info->minimum
															   max:(AUValue)p->info->maximum
															  unit:kAudioUnitParameterUnit_Generic
														  unitName:nil
															 flags:kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_CanRamp
													  valueStrings:nil
											   dependentParameters:nil];
		}

		[params addObject:param];
	}

	_parameterTree = [[AUParameterTree createTreeWithChildren:params] retain];
	[params release];

	AU3Core* corePtr = core.get();

	_parameterTree.implementorValueObserver = ^(AUParameter* param, AUValue value)
	{
		// Host automation may arrive off-main; the controller store and the
		// ui->dsp queue are main-thread-owned, so hop when needed.
		const auto address = param.address;
		if ([NSThread isMainThread])
		{
			corePtr->onParameterFromHost(address, value);
		}
		else
		{
			dispatch_async(dispatch_get_main_queue(), ^{
				corePtr->onParameterFromHost(address, value);
			});
		}
	};

	// Deliberately NO implementorValueProvider. The observer above may defer
	// its store update to the main thread, and a host that sets a value and
	// reads it straight back (auval does; Ableton famously does - see the AU2
	// wrapper's GetParameter comment) must see the value it just set. Without
	// a provider AUParameter answers from its own cache, which is always the
	// last value set by host or - via notifyDaw below - by our editor.

	// Seed initial values from the parameter defaults. The observer fires but
	// change-detection makes it a no-op beyond a harmless queue frame.
	{
		int i = 0;
		for (auto p : core->gmpiController.nativeParams)
		{
			AUParameter* param = _parameterTree.allParameters[i++];
			param.value = (AUValue)p->valueReal();
		}
	}

	// Editor edits reach the DAW here: setPinFromUi has already stored the
	// value in the controller, so pushing it into the tree notifies the host's
	// observers and, via the implementor observer above, the processor.
	AUParameterTree* treePtr = _parameterTree;
	core->gmpiController.notifyDaw = [treePtr](gmpi::hosting::GmpiParameter* param)
	{
		assert(param->info->dawTag != -1); // should never be called for non-native param.

		AUParameter* p = [treePtr parameterWithAddress:(AUParameterAddress)param->info->dawTag];
		if (p)
			[p setValue:(AUValue)param->valueReal() originator:nil];
	};

	// The DSP->UI queue needs draining whether or not any editor is open.
	// Instantiation may be off-main; the timer wants the main run loop.
	dispatch_async(dispatch_get_main_queue(), ^{
		corePtr->startTimer(AU3Core::timerPeriodMs);
	});

	self.maximumFramesToRender = 512;

	return self;
}

- (void)dealloc
{
	core = nullptr;

	[_inputBusArray release];
	[_outputBusArray release];
	[_parameterTree release];

	[super dealloc];
}

- (gmpi::hosting::gmpi_controller_holder*)gmpiController
{
	return &core->gmpiController;
}

- (AUAudioUnitBusArray*)inputBusses
{
	return _inputBusArray;
}

- (AUAudioUnitBusArray*)outputBusses
{
	return _outputBusArray;
}

- (AUParameterTree*)parameterTree
{
	return _parameterTree;
}

- (BOOL)supportsMPE
{
	return YES;
}

- (MIDIProtocolID)AudioUnitMIDIProtocol
{
	return kMIDIProtocol_2_0;
}

- (NSDictionary<NSString*, id>*)fullState
{
	NSMutableDictionary* state = [[[super fullState] mutableCopy] autorelease];
	if (!state)
		state = [NSMutableDictionary dictionary];

	const auto chunk = core->gmpiController.getPreset();
	if (NSString* s = [NSString stringWithUTF8String:chunk.c_str()]; s)
		state[@"GMPIPRESET"] = s;

	return state;
}

- (void)setFullState:(NSDictionary<NSString*, id>*)fullState
{
	[super setFullState:fullState];

	NSString* s = fullState[@"GMPIPRESET"];
	if (![s isKindOfClass:[NSString class]])
		return;

	if (!core->gmpiController.setPresetXmlFromDaw([s UTF8String]))
		return;

	// Deliver the restored values everywhere they matter:
	//  * the processor, through its queue - drained at the top of the next
	//    render, or held until the first one if we're not yet initialised;
	//  * any open editor;
	//  * the AUParameterTree, so the host's own UI shows the preset. The
	//    observer is suppressed for these mirror-writes: the two stores
	//    already hold the value, so its work is already done.
	for (auto& [handle, param] : core->gmpiController.patchManager.parameters)
	{
		if (gmpi::hosting::HostControls::None != param.info->hostConnect || !param.info->is_stateful)
			continue;

		core->sendParameterToProcessorQueue(&param);
		core->gmpiController.notifyGui(&param);
	}

	core->suppressParameterObserver.store(true, std::memory_order_relaxed);
	for (auto p : core->gmpiController.nativeParams)
	{
		AUParameter* param = [_parameterTree parameterWithAddress:(AUParameterAddress)p->info->dawTag];
		if (param)
			[param setValue:(AUValue)p->valueReal() originator:nil];
	}
	core->suppressParameterObserver.store(false, std::memory_order_relaxed);
}

- (BOOL)allocateRenderResourcesAndReturnError:(NSError**)outError
{
	if (![super allocateRenderResourcesAndReturnError:outError])
		return NO;

	// Fixed channel config: refuse a host that negotiated something else.
	if (_outputBusArray.count > 0)
	{
		const auto negotiated = _outputBusArray[0].format.channelCount;
		if ((int)negotiated != core->outputCount)
		{
			if (outError)
				*outError = [NSError errorWithDomain:NSOSStatusErrorDomain code:kAudioUnitErr_FormatNotSupported userInfo:nil];
			return NO;
		}
	}

	core->maxFrames = (int32_t)self.maximumFramesToRender;
	core->sampleRate = (_outputBusArray.count > 0)
		? (float)_outputBusArray[0].format.sampleRate
		: 44100.0f;

	// Sample storage the render thread will hand the plugin: input pull
	// destinations, output fallbacks for null-mData hosts, silence for spare pins.
	core->inputStorage.assign(core->inputCount, std::vector<float>(core->maxFrames, 0.0f));
	core->outputStorage.assign(core->outputCount, std::vector<float>(core->maxFrames, 0.0f));
	core->dummyInputBuffer.assign(core->maxFrames, 0.0f);
	core->dummyOutputBuffer.assign(core->maxFrames, 0.0f);

	// A non-interleaved AudioBufferList shell over inputStorage, rebuilt every
	// render because pullInputBlock may replace the data pointers.
	if (core->inputCount > 0)
	{
		core->inputABLStorage.assign(
			offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * core->inputCount, 0);
	}

	if (self.musicalContextBlock)
		core->musicalContextBlock = [self.musicalContextBlock copy];

	auto& info = *gmpi::hosting::factory::getInstance().getPluginInfo();
	core->plugin.start_processor(core.get(), info, core->maxFrames, core->sampleRate);

	if (!core->plugin.processor)
	{
		if (outError)
			*outError = [NSError errorWithDomain:NSOSStatusErrorDomain code:kAudioUnitErr_FailedInitialization userInfo:nil];
		return NO;
	}

	core->processorIsInitialized = true;

	return YES;
}

- (void)deallocateRenderResources
{
	if (core->musicalContextBlock)
	{
		[core->musicalContextBlock release];
		core->musicalContextBlock = nil;
	}

	[super deallocateRenderResources];
}

- (AUInternalRenderBlock)internalRenderBlock
{
	// The block outlives self in some host teardown orders; capture the core
	// by raw pointer and nothing Objective-C.
	AU3Core* corePtr = core.get();
	auto& info = *gmpi::hosting::factory::getInstance().getPluginInfo();
	const gmpi::hosting::pluginInfo* infoPtr = &info;

	return [[^AUAudioUnitStatus(
		AudioUnitRenderActionFlags* actionFlags,
		const AudioTimeStamp* timestamp,
		AUAudioFrameCount frameCount,
		NSInteger outputBusNumber,
		AudioBufferList* outputData,
		const AURenderEvent* realtimeEventListHead,
		AURenderPullInputBlock pullInputBlock)
	{
		auto& plugin_ = corePtr->plugin.processor;
		auto& events = corePtr->plugin.events;

		if (!plugin_ || frameCount > (AUAudioFrameCount)corePtr->maxFrames)
		{
			// Silence rather than garbage.
			for (UInt32 i = 0; i < outputData->mNumberBuffers; ++i)
				if (outputData->mBuffers[i].mData)
					memset(outputData->mBuffers[i].mData, 0, outputData->mBuffers[i].mDataByteSize);
			return kAudioUnitErr_NoConnection;
		}

		// Parameter changes and presets from the main thread.
		corePtr->queueToDsp.pollMessage(&corePtr->plugin);

		// Tempo and musical time from the host, forwarded as host controls.
		if (corePtr->musicalContextBlock)
		{
			double currentTempo = 120.0;
			double timeSignatureNumerator = 4.0;
			NSInteger timeSignatureDenominator = 4;
			double currentBeatPosition = 0.0;
			NSInteger sampleOffsetToNextBeat = 0;
			double currentMeasureDownbeatPosition = 0.0;

			if (corePtr->musicalContextBlock(&currentTempo, &timeSignatureNumerator, &timeSignatureDenominator,
					&currentBeatPosition, &sampleOffsetToNextBeat, &currentMeasureDownbeatPosition))
			{
				if (!std::isfinite(currentBeatPosition))
					currentBeatPosition = 0.0;

				corePtr->plugin.setHostControlFromDaw(gmpi::hosting::HostControls::TimeBpm, currentTempo);
				corePtr->plugin.setHostControlFromDaw(gmpi::hosting::HostControls::TimeNumerator, timeSignatureNumerator);
				corePtr->plugin.setHostControlFromDaw(gmpi::hosting::HostControls::TimeDenominator, (double)timeSignatureDenominator);
				corePtr->plugin.setHostControlFromDaw(gmpi::hosting::HostControls::TimeQuarterNotePosition, currentBeatPosition);
			}
		}

		// Scheduled events: sample-accurate parameters and MIDI.
		for (const AURenderEvent* event = realtimeEventListHead; event; event = event->head.next)
		{
			const auto sampleOffset = std::max<int>(0,
				(int)(event->head.eventSampleTime - (AUEventSampleTime)timestamp->mSampleTime));

			switch (event->head.eventType)
			{
			case AURenderEventParameter:
			case AURenderEventParameterRamp:
			{
				const auto& pe = event->parameter;
				if (pe.parameterAddress < corePtr->plugin.nativeParams.size())
				{
					auto p = corePtr->plugin.nativeParams[pe.parameterAddress];
					// sendToEditor: the change came from the host, so the GUI
					// hears about it through the dsp->ui queue - the CLAP pattern.
					corePtr->plugin.setParameterNormalizedFromDaw(
						*infoPtr, sampleOffset, p->info->id, p->real2Normalized(pe.value), true);
				}
			}
			break;

			case AURenderEventMIDI:
			{
				// MIDI 1.0 bytes; MidiConverter2 converts to MIDI 2.0.
				const auto& me = event->MIDI;
				corePtr->midiConverter->processMidi({ me.data, me.length }, sampleOffset);
			}
			break;

			case AURenderEventMIDIEventList:
			{
				const MIDIEventList& list = event->MIDIEventsList.eventList;
				const MIDIEventPacket* packet = &list.packet[0];
				for (UInt32 i = 0; i < list.numPackets; ++i)
				{
					processUmpWords(*corePtr, packet->words, packet->wordCount,
						sampleOffset + (int)packet->timeStamp);
					packet = MIDIEventPacketNext(packet);
				}
			}
			break;

			default:
				break;
			}
		}

		// Pull input.
		int validInputChannels = 0;
		if (corePtr->inputCount > 0 && pullInputBlock)
		{
			auto* inABL = reinterpret_cast<AudioBufferList*>(corePtr->inputABLStorage.data());
			inABL->mNumberBuffers = corePtr->inputCount;
			for (int i = 0; i < corePtr->inputCount; ++i)
			{
				inABL->mBuffers[i].mNumberChannels = 1;
				inABL->mBuffers[i].mDataByteSize = frameCount * sizeof(float);
				inABL->mBuffers[i].mData = corePtr->inputStorage[i].data();
			}

			AudioUnitRenderActionFlags pullFlags = 0;
			const auto err = pullInputBlock(&pullFlags, timestamp, frameCount, 0, inABL);
			if (err == noErr)
			{
				for (UInt32 i = 0; i < inABL->mNumberBuffers && validInputChannels < (int)corePtr->inputPtr.size(); ++i)
					corePtr->inputPtr[validInputChannels++] = (float*)inABL->mBuffers[i].mData;
			}
		}
		for (int i = validInputChannels; i < (int)corePtr->inputPtr.size(); ++i)
			corePtr->inputPtr[i] = corePtr->dummyInputBuffer.data();

		// Output: host buffers when provided, ours when mData is null.
		int validOutputChannels = 0;
		for (UInt32 i = 0; i < outputData->mNumberBuffers && validOutputChannels < (int)corePtr->outputPtr.size(); ++i)
		{
			if (!outputData->mBuffers[i].mData)
				outputData->mBuffers[i].mData = corePtr->outputStorage[validOutputChannels].data();

			corePtr->outputPtr[validOutputChannels++] = (float*)outputData->mBuffers[i].mData;
		}
		for (int i = validOutputChannels; i < (int)corePtr->outputPtr.size(); ++i)
			corePtr->outputPtr[i] = corePtr->dummyOutputBuffer.data();

		// Hand the buffers to the plugin's pins, in pin order - same walk as AU2.
		{
			int inIdx = 0;
			int outIdx = 0;
			for (auto& pin : infoPtr->dspPins)
			{
				if (pin.datatype != gmpi::PinDatatype::Audio)
					continue;

				if (pin.direction == gmpi::PinDirection::In)
					plugin_->setBuffer(pin.id, corePtr->inputPtr[inIdx++]);
				else
					plugin_->setBuffer(pin.id, corePtr->outputPtr[outIdx++]);
			}
		}

		plugin_->process(frameCount, events.head());
		events.clear();

		// Parameter updates from the processor toward the UI.
		if (corePtr->plugin.pendingControllerQueueClients.ServiceWaitersIncremental(
				&corePtr->gmpiController.message_que_dsp_to_ui, frameCount))
		{
			corePtr->gmpiController.message_que_dsp_to_ui.Send();
		}

		return noErr;
	} copy] autorelease];
}

@end
