#include "Processor_VST3.h"
#include "Controller_VST3.h"   // E68 - the paired-controller pull
#include "StateStreamRead.h"
//#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/base/ibstream.h"
#include <algorithm>
#include "wrapper/common/unicode_conversion.h"
#include "GmpiMidi.h"

using namespace wrapper::JmUnicodeConversions;

#if defined( _WIN32)
extern HINSTANCE ghInst;
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

typedef gmpi::ReturnCode(*MP_DllEntry)(void**);

namespace wrapper
{

//-----------------------------------------------------------------------------
Processor_VST3::Processor_VST3 (gmpi::hosting::pluginInfo& pinfo)
: active_(false)
, info(pinfo)
, outputsAsStereoPairs(true)
, m_message_que_dsp_to_ui(0x500000)//AUDIO_MESSAGE_QUE_SIZE) //TODO
, m_message_que_ui_to_dsp(0x500000)//AUDIO_MESSAGE_QUE_SIZE)
, midiConverter(
	// provide a lambda to accept converted MIDI 2.0 messages
	[this](const gmpi::midi2::message_view& msg, int offset)
	{
		const auto header = gmpi::midi2::decodeHeader(msg);

		// only 8-byte messages supported. only 16 channels supported
		if (header.messageType != gmpi::midi2::ChannelVoice64 || header.channel > 15)
			return;

		// only within process(), and the host may provide no event list.
		auto outputEvents = dataptr ? dataptr->outputEvents : nullptr;
		if (!outputEvents)
			return;

		// Prepare a VST3 event
		const uint16_t eventNotSet = 12345;
		Steinberg::Vst::Event event{};
		event.type = eventNotSet;
		event.busIndex = midiOutBus_;
		event.sampleOffset = offset;

		// Parse MIDI 2.0 into a VST3 event
		switch (header.status)
		{
		case gmpi::midi2::NoteOn:
		{
			const auto note = gmpi::midi2::decodeNote(msg);
#if 0
			uint8_t keyNumber = note.noteNumber & 0x7f; // clamp to range [0 - 127] 

			// derive keynumber from pitch, since noteNumber may have no relation to pitch.
			if (gmpi::midi2::attribute_type::Pitch == note.attributeType)
			{
			}
#endif
			const auto keyNumber = static_cast<uint8_t>(static_cast<int>(floorf(0.5f + midi2NoteTune[note.noteNumber])) & 0x7f);

			midi2NoteToKey[note.noteNumber] = keyNumber;

			event.type = Steinberg::Vst::Event::kNoteOnEvent;
			event.noteOn.noteId = keyNumber;
			event.noteOn.pitch = keyNumber;
			event.noteOn.velocity = note.velocity;
			event.noteOn.channel = header.channel & 0x0f;
		}
		break;

		case gmpi::midi2::NoteOff:
		{
			// TODO: !!! lookup nearest key number based on pitch, store it for note-off.
			const auto note = gmpi::midi2::decodeNote(msg);

			event.type = Steinberg::Vst::Event::kNoteOffEvent;
			event.noteOff.pitch = midi2NoteToKey[note.noteNumber];
			event.noteOff.noteId = midi2NoteToKey[note.noteNumber];
			event.noteOff.velocity = note.velocity;
			event.noteOff.channel = header.channel & 0x0f;

			// reset pitch of note number.
			midi2NoteToKey[note.noteNumber] = note.noteNumber;
		}
		break;

		case gmpi::midi2::ControlChange:
		{
			const auto controller = gmpi::midi2::decodeController(msg);
			if (controller.type < 128)
			{
				const auto newQuantizedValue = gmpi::midi_utils::floatToU7(controller.value);

				// 'thin' repeated 7-bit values. Unless it apears to be on purpose (e.g. all notes off)
				// Send exact (deliberate) repeats, but not 'close' (different but close unquantized) repeats
				if(ControlChangeValue[controller.type].quantized != newQuantizedValue || controller.value == ControlChangeValue[controller.type].unquantized)
				{
					ControlChangeValue[controller.type].quantized = newQuantizedValue;
					ControlChangeValue[controller.type].unquantized = controller.value;

					event.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
					event.midiCCOut.controlNumber = controller.type;
					event.midiCCOut.value = gmpi::midi_utils::floatToU7(controller.value);
					event.midiCCOut.channel = header.channel & 0x0f;
				}
			}
		}
		break;

		// track pitch changes
		case gmpi::midi2::PolyControlChange:
		{
			const auto polyController = gmpi::midi2::decodePolyController(msg);

			if (polyController.type == gmpi::midi2::PolyPitch)
			{
				const auto semitones = gmpi::midi2::decodeNotePitch(msg);
				midi2NoteTune[polyController.noteNumber] = semitones;
			}
		}
		break;

		case gmpi::midi2::ChannelPressue:
		{
			const auto normalized = gmpi::midi2::decodeController(msg).value;

			event.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
			event.midiCCOut.controlNumber = Steinberg::Vst::kAfterTouch;
			event.midiCCOut.value = gmpi::midi_utils::floatToU7(normalized);
			event.midiCCOut.channel = header.channel & 0x0f;
		}
		break;

		case gmpi::midi2::PitchBend:
		{
			const auto normalized = gmpi::midi2::decodeController(msg).value;

			event.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
			event.midiCCOut.controlNumber = Steinberg::Vst::kPitchBend;
			event.midiCCOut.channel = header.channel & 0x0f;
			gmpi::midi_utils::normalizedToBipoler14bit(
				normalized
				, (uint8_t&)event.midiCCOut.value
				, (uint8_t&)event.midiCCOut.value2
			);
		}
		break;

		case gmpi::midi2::PolyAfterTouch:
		{
			const auto aftertouch = gmpi::midi2::decodePolyController(msg);

#if 0
			// plain CC
			event.type = Steinberg::Vst::Event::kLegacyMIDICCOutEvent;
			event.midiCCOut.controlNumber = kCtrlPolyPressure;
			event.midiCCOut.value = aftertouch.noteNumber;
			event.midiCCOut.value2 = gmpi::midi_utils::floatToU7(aftertouch.value);
#else
			// VST3 poly pressure event
			event.type = Steinberg::Vst::Event::kPolyPressureEvent;
			event.polyPressure.pitch = aftertouch.noteNumber;
			event.polyPressure.noteId = aftertouch.noteNumber;
			event.polyPressure.pressure = aftertouch.value;
			event.polyPressure.channel = header.channel & 0x0f;
#endif
		}
		break;

		case gmpi::midi2::System: // SYSTEM_EXCLUSIVE:
		{
#if 0 // TODO
			event.data.type = Steinberg::Vst::DataEvent::kMidiSysEx;

			// bit of a hack. the sysex data is technically 'freed' and *might* be overwritten at some future point when buffer wraps.
			event.data.bytes = midi_msg->data;
			event.data.size = midi_msg->size;
#endif
		}
		break;

		default:
			break;
		};

		// pass event to DAW
		if (event.type != eventNotSet)
		{
			outputEvents->addEvent(event);
		}
	}
)
{
	killBackgroundthread.store(false);
//TODO	memset( &timeInfo, 0, sizeof( timeInfo ) );

	for (int c = 0; c < std::size(noteIds); ++c)
	{
		for (int n = 0; n < std::size(noteIds[0]); ++n)
		{
			noteIds[c][n].MidiKeyNumber = n;
			noteIds[c][n].pitch2 = n;
			noteIds[c][n].channel = c;
			noteIds[c][n].noteId = -1;
		}
	}

	for (size_t i = 0; i < std::size(midi2NoteToKey); ++i)
	{
		midi2NoteToKey[i] = static_cast<uint8_t>(i);
		midi2NoteTune[i] = static_cast<float>(i);
	}

	for (auto& q : ControlChangeValue)
	{
		// out-of-range value to trigger initial update.
		q.quantized = 255;
		q.unquantized = -1.f;
	}

	sysexOut_.assign(0x10000, 0);
	plugin.midiOutputSink = this;
    
    plugin.init(info);

	seMsgQues_ = dynamic_cast<IProcessorMessageQues*>(plugin.processor.get());
	if (seMsgQues_)
		seDspToUiQueue_ = dynamic_cast<gmpi::hosting::lock_free_fifo*>(seMsgQues_->MessageQueToGui());
}

Processor_VST3::~Processor_VST3 ()
{
	if (background.joinable())
	{
		{
			std::unique_lock<std::mutex> lk(backgroundMutex);
			killBackgroundthread.store(true);
		}
		backgroundSignal.notify_one();

		background.join();
	}

//	gmpi_dynamic_linking::MP_DllUnload(plugin_dllHandle_to_unload);
}

void Processor_VST3::reInitialise()
{
	silence.assign(processSetup.maxSamplesPerBlock, 0.0f);

	plugin.start_processor(&plugin, info, processSetup.maxSamplesPerBlock, processSetup.sampleRate);
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API Processor_VST3::initialize (FUnknown* context)
{
	tresult result = AudioEffect::initialize(context);
	if (result != kResultTrue)
		return result;

	outputsAsStereoPairs = true; // TODO .factory->GetOutputsAsStereoPairs();

	//const auto& info = BundleInfo::instance()->getPluginInfo();
	//supportedChannels = info.support16MidiChannels ? 16 : 1;

	reInitialise();

	{
		int numInputs = countPins(info, gmpi::PinDirection::In, gmpi::PinDatatype::Audio);
		inputBuffers.assign(numInputs, nullptr);

		int pinIndex = 0;
		if (outputsAsStereoPairs)
		{
			while (numInputs > 1)
			{
				const auto name = ToUtf16(getPinName(info, gmpi::PinDirection::In, pinIndex));

				addAudioInput((const TChar*) name.c_str(), SpeakerArr::kStereo);
				numInputs -= 2;
				pinIndex += 2;
			}
		}

		while (numInputs > 0)
		{
			const auto name = ToUtf16(getPinName(info, gmpi::PinDirection::In, pinIndex));
			addAudioInput((const TChar*) name.c_str(), SpeakerArr::kMono);
			numInputs -= 1;
			pinIndex += 1;
		}
	}

	{
		int numOutputs = countPins(info, gmpi::PinDirection::Out, gmpi::PinDatatype::Audio);
		outputBuffers.assign(numOutputs, nullptr);
		int pinIndex = 0;
		if (outputsAsStereoPairs)
		{
			while (numOutputs > 1)
			{
				const auto name = ToUtf16(getPinName(info, gmpi::PinDirection::Out, pinIndex));
				addAudioOutput((const TChar*) name.c_str(), SpeakerArr::kStereo);
				numOutputs -= 2;
				pinIndex += 2;
			}
		}
		while (numOutputs > 0)
		{
			const auto name = ToUtf16(getPinName(info, gmpi::PinDirection::Out, pinIndex));
			addAudioOutput((const TChar*) name.c_str(), SpeakerArr::kMono);
			numOutputs -= 1;
			pinIndex += 1;
		}
	}

	for (int i = countPins(info, gmpi::PinDirection::In, gmpi::PinDatatype::Midi) ; i > 0 ; --i)
	{
		addEventInput(STR16 ("MIDI In"), 16);
	}

	for (int i = countPins(info, gmpi::PinDirection::Out, gmpi::PinDatatype::Midi); i > 0; --i)
	{
		addEventOutput(STR16("MIDI Out"), 16);
	}

	if (!background.joinable()) // protect against double initialize (Steinberg validator)
	{
		background = std::thread(
			[this]
			{
				CommunicationProc();
			}
			);
	}

	return result;
}

uint32 Processor_VST3::getLatencySamples()
{
	const auto latency = plugin.getLatency();
	latencyReadByHost_.store(latency, std::memory_order_relaxed);
	auto notified = latency;
	latencyNotified_.compare_exchange_strong(notified, -1); // the host has caught up with our notification.
	return static_cast<uint32>(latency);
}

//-----------------------------------------------------------------------------
// Not called?
tresult PLUGIN_API Processor_VST3::setBusArrangements (SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts)
{
	// we only support one in and output bus and these buses must have the same number of channels
	if (numIns == 1 && numOuts == 1 && inputs[0] == outputs[0])
		return AudioEffect::setBusArrangements (inputs, numIns, outputs, numOuts);

	// Otherwise accept exactly the buses we already have, e.g. an instrument's: no inputs, its outputs.
	auto matches = [this](BusDirection dir, const SpeakerArrangement* requested, int32 count)
	{
		if (count != getBusCount(kAudio, dir))
			return false;
		for (int32 i = 0; i < count; ++i)
		{
			SpeakerArrangement current{};
			if (getBusArrangement(dir, i, current) != kResultTrue || current != requested[i])
				return false;
		}
		return true;
	};

	return matches(kInput, inputs, numIns) && matches(kOutput, outputs, numOuts) ? kResultTrue : kResultFalse;
}

//-----------------------------------------------------------------------------
tresult PLUGIN_API Processor_VST3::setActive (TBool state)
{
/* Seems to be checking number of output channels. SE does not require any particular number.
	SpeakerArrangement arr;
	if (getBusArrangement (kOutput, 0, arr) != kResultTrue)
		return kResultFalse;
	int32 numChannels = SpeakerArr::getChannelCount (arr);
	if (numChannels == 0)
		return kResultFalse;
*/

//	_RPT1( _CRT_WARN, "SeProcessor::setActive (%d)n", state );

	if (state)
	{
		reInitialise();
	}

	active_ = state != 0;

	return AudioEffect::setActive (state);
}

#if 0
// VST3 just uses successive incrementing id for notes, this maps those values to a key.
// by default we use the natural key for that pitch, else we find a spare key.
int SeProcessor::allocateKey(int noteId, int pitchSemiTone)
{
	const int naturalKey = 0x7f & pitchSemiTone;

	int key = naturalKey;

	if (heldNotes[naturalKey])
	{
		for (int i = 0; i < 128; ++i)
		{
			key = (key + 1) & 0x7f;

			if (!heldNotes[key])
				break;
		}
	}
	
	heldNotes[key] = true;
	noteIds[key] = noteId;

	return key;
}
#endif
// A host may reuse a released note's ID, so a held key wins over a released one.
Processor_VST3::vstNoteInfo* Processor_VST3::findKey(uint8_t channel, int noteId, bool heldOnly)
{
	if (noteId < 0) // -1 marks an unused key, stored IDs are never negative
		return {};

	vstNoteInfo* released{};
	for (auto& keyInfo : noteIds[channel])
	{
		if (keyInfo.noteId == noteId)
		{
			if (keyInfo.held)
				return &keyInfo;

			if (!released)
				released = &keyInfo;
		}
	}

	return heldOnly ? nullptr : released;
}

// Note-expression events carry a note ID but no channel, so look on every channel, preferring a sounding key.
Processor_VST3::vstNoteInfo* Processor_VST3::findKeyOnAnyChannel(int noteId)
{
	vstNoteInfo* released{};
	for (uint8_t channel = 0; channel < std::size(noteIds); ++channel)
	{
		if (auto keyInfo = findKey(channel, noteId, true); keyInfo)
			return keyInfo;

		if (!released)
			released = findKey(channel, noteId);
	}

	return released;
}

Processor_VST3::vstNoteInfo& Processor_VST3::allocateKey(const NoteOnEvent& note)
{
	auto& ret = noteIds[note.channel][note.pitch];

	ret.held = true;

	if(note.noteId < 0)
		ret.noteId = note.pitch;
	else
		ret.noteId = note.noteId;

	return ret;

#if 0
	// TODO!! use allocation mode here base on host-control HC_MPE_MODE:
	// 1. assume source is a standard keyboard, and that two different noteIds with same pitch means same key/voice (to enable soft/hard steal to work)
	// 2. MPE assume two different noteIds with same pitch are independent voices. and only same noteID means same key/voice.
	const auto hint = VoiceAllocationHint::Keyboard;

	if (e.noteOn.noteId == -1) // note id increments indefinitely each note.
		e.noteOn.noteId = e.noteOn.pitch;


	// prefer to allocate voice as per MIDI 1.0 (semitone = key)
	if (VoiceAllocationHint::Keyboard == hint)
	{
		return noteIds[semitone];
	}
	// prefer to allocate voice with same noteId.
	for (int i = 0; i < 128; ++i)
	{
		if (noteIds[i].noteId == noteId)
		{
			return noteIds[i];
		}
	}

	// allocate in a round robin fashion.
	for (int i = 0; i < 128; ++i)
	{
		noteIdsRoundRobin = (noteIdsRoundRobin + 1) & 0x7f;

		if (!noteIds[noteIdsRoundRobin].held)
		{
			break;
		}
	}

	return noteIds[noteIdsRoundRobin];
#endif
}

void Processor_VST3::MidiIn(int sampleOffset, const uint8_t* data, int32_t size)
{
	assert(plugin.MidiInputPinIdx > -1);

	gmpi::api::Event ge
	{
		{},							// next (populated later)
		sampleOffset,				// timeDelta
		gmpi::api::EventType::Midi,
		plugin.MidiInputPinIdx,			// pinIdx
		size,						// size_
		{}							// data_/oversizeData_
	};

	auto dst = reinterpret_cast<uint8_t*>(&ge.data_);
	std::copy(data, data + size, dst);

	plugin.events.push(ge);
}

//-----------------------------------------------------------------------------

tresult PLUGIN_API Processor_VST3::process (ProcessData& data)
{
	auto& plugin_ = plugin.processor;
	auto& events = plugin.events;

    assert(!plugin_.isNull());

	m_message_que_ui_to_dsp.pollMessage(&plugin);

	if (data.inputParameterChanges)
	{
		int32 paramChangeCount = data.inputParameterChanges->getParameterCount ();
		for (int32 index = 0; index < paramChangeCount; index++)
		{
			IParamValueQueue* queue = data.inputParameterChanges->getParameterData (index);
			if (queue && queue->getParameterId () >= 0 )
			{
				int32 valueChangeCount = queue->getPointCount ();
				ParamValue valueNormalized;
				int32 sampleOffset{};
				const int paramId = queue->getParameterId();

				if (paramId < MidiControllersParameterId)
				{
					if (queue->getPoint (valueChangeCount-1, sampleOffset, valueNormalized) == kResultTrue)
					{
						auto prev_sampleOffset{ sampleOffset };
						//_RPTW1( _CRT_WARN, L"Processor : %f\n", value );
						assert(sampleOffset >= prev_sampleOffset && sampleOffset < data.numSamples);
//						_RPT1(_CRT_WARN, "                     PRESETS-DSP: P%d Set in Process()\n", id);

						plugin.setParameterNormalizedFromDaw(info, sampleOffset, paramId, valueNormalized);
					}
				}
				else if (plugin.MidiInputPinIdx > -1)
				{
					// CC, channel pressure and pitch bend the host mapped onto parameters (Controller_VST3::getMidiControllerAssignment). Every point, so a bend keeps its shape within the block.
					constexpr int ccsPerChannel = 130;
					const auto channel = (paramId - MidiControllersParameterId) / ccsPerChannel;
					const auto cc = (paramId - MidiControllersParameterId) % ccsPerChannel;

					for (int32 point = 0; channel < 16 && point < valueChangeCount; ++point)
					{
						if (queue->getPoint(point, sampleOffset, valueNormalized) != kResultTrue)
							continue;

						gmpi::midi::message64 msgout;

						if(cc < 128 )
						{
							msgout = gmpi::midi2::makeController(
								static_cast<uint8_t>(cc)
								, valueNormalized
								, channel
							);
						}
						else
						{
							switch(cc)
							{
								case 128: // Channel Pressure.
								{
									msgout = gmpi::midi2::makeChannelPressure(
										valueNormalized
										, channel
									);
								}
								break;

								case 129: // Bender.
								{
//										_RPTN(0, "C%2d BDR %f\n", channel, value);
									msgout = gmpi::midi2::makeBender(
										valueNormalized // use normalized bender
										, channel
									);
								}
								break;
							}
						}

						MidiIn(
							  sampleOffset
							, msgout.m
							, sizeof(msgout.m)
						);
					}
				}
			}
		}
	}

	if (data.numSamples <= 0)
		return kResultTrue;

	if (plugin.MidiInputPinIdx > -1)
	{
		const int32 numEvents = data.inputEvents ? data.inputEvents->getEventCount() : 0;
		
		for (int32 eventIndex = 0; eventIndex < numEvents; ++eventIndex)
		{
			Event e;
			if (data.inputEvents->getEvent(eventIndex, e) != kResultTrue)
			{
				break;
			}

			switch (e.type)
			{
			case Event::kNoteOnEvent:
			{
				// handle hosts that don't pass a velocity=0 note-on as a note-off
				if (e.noteOn.velocity == 0.0f)
				{
					if (e.noteOn.noteId == -1)
						e.noteOn.noteId = e.noteOn.pitch;

					DoNoteOff(e.noteOn.channel, e.noteOn.noteId, e.noteOn.pitch, e.noteOn.velocity, e.sampleOffset);
				}
				else
				{
					auto& keyInfo = allocateKey(e.noteOn);

					// In the case a non natural key was allocated, send pitch information also. (else SE will play wrong pitch)
					const float pitch = e.noteOn.pitch + 0.01f * e.noteOn.tuning; // microtunable pitch
					if (keyInfo.pitch2 != pitch)
					{
						keyInfo.pitch2 = pitch;

						const auto out = gmpi::midi2::makeNotePitchMessage(
							keyInfo.MidiKeyNumber,
							keyInfo.pitch2,
							keyInfo.channel
						);

						MidiIn(e.sampleOffset, (const unsigned char*)&out, sizeof(out));
					}

					// TODO : DON"T SEND UNLESS WE HAVE TO (MAYBE RESET THEM AUTOMATICALLY IN PM on note on
					// reset note expression controllers.
					{
						for (auto controllerId : { gmpi::midi2::PolyVolume, gmpi::midi2::PolyPan, gmpi::midi2::PolySoundController5 })
						{
							const auto msg = gmpi::midi2::makePolyController(
								keyInfo.MidiKeyNumber,
								controllerId,
								0.0f,
								keyInfo.channel
							);

							MidiIn(e.sampleOffset, (const unsigned char*)&msg, sizeof(msg));
						}
						// per-note bender
						{
							const auto msg = gmpi::midi2::makePolyBender(
								keyInfo.MidiKeyNumber,
								0.5f,
								keyInfo.channel
							);

							MidiIn(e.sampleOffset, (const unsigned char*)&msg, sizeof(msg));
						}
					}

					{
						// _RPTN(0, "C%2d NON %3d\n", e.noteOn.channel, e.noteOn.pitch);

						const auto out = gmpi::midi2::makeNoteOnMessage(
							keyInfo.MidiKeyNumber,
							e.noteOn.velocity,
							keyInfo.channel
						);

						MidiIn(e.sampleOffset, (const unsigned char*)&out, sizeof(out));
					}
				}
			}
			break;

			case Event::kNoteOffEvent:
			{
				if (e.noteOff.noteId == -1)
					e.noteOff.noteId = e.noteOff.pitch;

				DoNoteOff(e.noteOff.channel, e.noteOff.noteId, e.noteOff.pitch, e.noteOff.velocity, e.sampleOffset);
			}
			break;

			case Event::kPolyPressureEvent:
			{
				// noteid is always -1, and pitch 0
				if (e.polyPressure.noteId == -1)
					e.polyPressure.noteId = e.polyPressure.pitch;

				// if we have remapped this notenumber, use the remapped value. Otherwise use the default.
				int key = e.polyPressure.pitch;
				int channel = e.polyPressure.channel;
				if (auto keyInfo = findKey(channel, e.polyPressure.noteId); keyInfo)
				{
					key = keyInfo->MidiKeyNumber;
					channel = keyInfo->channel;
				}

				const auto out = gmpi::midi2::makePolyPressure(
					key,
					e.polyPressure.pressure,
					channel
				);

				MidiIn(
					e.sampleOffset,
					(const unsigned char*)&out,
					sizeof(out)
				);
			}
			break;

			case Event::kNoteExpressionValueEvent:
			{
				auto keyInfo = findKeyOnAnyChannel(e.noteExpressionValue.noteId);

				if (!keyInfo)
					break;

				if (kTuningTypeID == e.noteExpressionValue.typeId)
				{
					const double pitchBendRangeExpr = 120.0; // +/- 10 octaves
					constexpr double pitchBendRangeMidi = 24.0; // assumed
					constexpr double pitchBendRangeMidiInv = 1.0 / pitchBendRangeMidi;

					const double semitones = (e.noteExpressionValue.value - 0.5) * 2.0 * pitchBendRangeExpr;
					const double normalized = (std::max)(0.0, std::min(1.0, 0.5 + 0.5 * semitones * pitchBendRangeMidiInv));

//					_RPTN(0, "makePolyBender %f => %f semitones, %f normal\n", e.noteExpressionValue.value, semitones, normalized);

					// Send MIDI HD-Protocol Note Expression message.
					const auto msg = gmpi::midi2::makePolyBender(
						keyInfo->MidiKeyNumber,
						static_cast<float>(normalized),
						keyInfo->channel
					);

					MidiIn(e.sampleOffset, (const unsigned char*)&msg, sizeof(msg));
				}
				else
				{
					// PolyModulation and PolyBreath not accounted for, probley gonna be pretty common.

					// Volume, Pan, Tuning is all Cubase supports by default. (plus poly pressure)
					// probly need brightness too for MPE

					int controllerId = -1;
					switch (e.noteExpressionValue.typeId)
					{
					case kVolumeTypeID:
						controllerId = gmpi::midi2::PolyVolume;
						break;
					case kPanTypeID:
						controllerId = gmpi::midi2::PolyPan;
						break;
					case kExpressionTypeID: // offered by Controller_VST3, MPE pressure
						controllerId = gmpi::midi2::PolyExpression;
						break;
					case kBrightnessTypeID:
						controllerId = gmpi::midi2::PolySoundController5; // Brightness. MPE Vertical (Y)
						break;

					default:
						break;
					}

					// _RPTN(0, "EXPR: key %3d type %2d value %f\n", keyInfo->MidiKeyNumber, e.noteExpressionValue.typeId, e.noteExpressionValue.value);

					if (controllerId >= 0)
					{
						// Bitwig sends brightness as bipolar (-0.5 -> +0.5), Cubase as normalized (0 -> 1.0).
						// Not sure which is right.
						const auto safeValue = (std::min)(1.0, (std::max)(0.0, e.noteExpressionValue.value));

						// Send MIDI HD-Protocol Note Expression message.
						const auto msg = gmpi::midi2::makePolyController(
							keyInfo->MidiKeyNumber,
							controllerId,
							safeValue,
							keyInfo->channel
						);

						MidiIn(e.sampleOffset, (const unsigned char*)&msg, sizeof(msg));
					}
				}
			}
			break;

			case Event::kDataEvent:
			{
				if (e.data.type == DataEvent::kMidiSysEx)
				{
					bool isFirst = true;
					int remain = e.data.size - 2; // skip leading F0 and trailing F7
					const uint8_t* src = e.data.bytes + 1; // skip leading F0
					while (remain > 0)
					{
						const auto msg = gmpi::midi2::makeSysex(src, remain, isFirst);
						MidiIn(e.sampleOffset, (const unsigned char*)&msg, sizeof(msg));
					}
				}
			}
			break;
			}
		}
	}

	// Musical time. A field whose validity bit the host clears is left out, so the plugin keeps its last good value.
	if (data.processContext)
	{
		using gmpi::hosting::HostControls;
		const auto& vst3Time = *data.processContext;

		if ((vst3Time.state & ProcessContext::kTempoValid) != 0)
			plugin.setHostControlFromDaw(HostControls::TimeBpm, vst3Time.tempo);

		if ((vst3Time.state & ProcessContext::kTimeSigValid) != 0)
		{
			plugin.setHostControlFromDaw(HostControls::TimeNumerator, vst3Time.timeSigNumerator);
			plugin.setHostControlFromDaw(HostControls::TimeDenominator, vst3Time.timeSigDenominator);
		}

		if ((vst3Time.state & ProcessContext::kProjectTimeMusicValid) != 0)
			plugin.setHostControlFromDaw(HostControls::TimeQuarterNotePosition, vst3Time.projectTimeMusic);

		if ((vst3Time.state & ProcessContext::kBarPositionValid) != 0)
			plugin.setHostControlFromDaw(HostControls::TimeBarStart, vst3Time.barPositionMusic);

		plugin.setHostControlFromDaw(HostControls::TimeTransportPlaying, (vst3Time.state & ProcessContext::kPlaying) != 0 ? 1.0 : 0.0);
	}

	// 2 = offline, as SynthEdit's HC_PROCESS_RENDERMODE; sent only on a change, and only a plug-in with a Process/Rendermode pin sees it.
	plugin.setHostControlFromDaw(gmpi::hosting::HostControls::ProcessRendermode, data.processMode == kOffline ? 2.0 : 0.0);

	// Setup audio buffers.
	int numChannelsIn = 0;
	int n = 0;
	for (int bus = 0; bus < data.numInputs; ++bus)
	{
		auto silenceFlags = data.inputs[bus].silenceFlags;
		numChannelsIn += data.inputs[bus].numChannels;
		const auto channels = (std::min)((size_t) data.inputs[bus].numChannels, inputBuffers.size());

		for (int channel = 0; channel < channels; ++channel)
		{
			const bool isSilent = 0 != (silenceFlags & 1);

//			synthEditProject.setInputSilent(n, isSilent);

			if (isSilent)
			{
				// Cubase sends non-silent buffers with silent flag set. In this case. substitute a actually silent buffer.
				inputBuffers[n] = silence.data();
			}
			else
			{
	//			assert(0 == (silenceFlags & 1) || *inputBuffers[n] == 0.0f); // check 'silent' buffers are zeroed
				inputBuffers[n] = data.inputs[bus].channelBuffers32[channel];
			}

			++n;
			silenceFlags = silenceFlags >> 1;
		}
	}

	int numChannelsOut = 0;
	n = 0;
	for (int bus = 0; bus < data.numOutputs; ++bus)
	{
		numChannelsOut += data.outputs[bus].numChannels;
		for (int channel = 0; channel < data.outputs[bus].numChannels && n < outputBuffers.size(); ++channel)
		{
			outputBuffers[n++] = data.outputs[bus].channelBuffers32[channel];
		}
	}

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
				plugin_->setBuffer(pin.id, inputBuffers[inIdx++]);
			}
			else
			{
				plugin_->setBuffer(pin.id, outputBuffers[outIdx++]);
			}
		}

		assert(inIdx == inputBuffers.size());
		assert(outIdx == outputBuffers.size());
	}

	// Process audio. MIDI the plugin sends arrives meanwhile via onMidiOut().
	dataptr = &data;
	sysexOutUsed_ = 0;
	plugin_->process(data.numSamples, events.head());
	dataptr = {};

	events.clear();

	// Latency the plugin reported that the host has neither read nor been told of.
	// restartComponent is UI-thread only, so the background thread messages the controller.
	if (const auto latency = plugin.getLatency(); latency != latencyReadByHost_.load(std::memory_order_relaxed) && latency != latencyNotified_.load(std::memory_order_relaxed))
	{
		latencyNotified_.store(latency, std::memory_order_relaxed);
		latencyChangePending_.store(true, std::memory_order_release);
	}
	if (latencyChangePending_.load(std::memory_order_acquire))
		onQueDataAvailable(); // re-signalled every block until taken: the background thread's wait can miss one.

	// messages from Processor -> Editor
	plugin.pendingControllerQueueClients.ServiceWaitersIncremental(
		  &m_message_que_dsp_to_ui
		, data.numSamples
	);

	if (m_message_que_dsp_to_ui.readyBytes())
	{
		onQueDataAvailable();
	}

	// SynthEdit engine feedback (see seMsgQues_'s declaration comment). The engine itself
	// already wrote any new bytes directly into this queue, via SeGmpiProcessor's connectPeer
	// at construction - this just wakes the background thread that forwards them.
	if (seDspToUiQueue_ && seDspToUiQueue_->readyBytes())
	{
		onQueDataAvailable();
	}

	return kResultTrue;
}

void Processor_VST3::DoNoteOff(int channel, int32_t noteId, int32_t key, float velocity, int sampleOffset)
{
	if (auto keyInfo = findKey(channel, noteId, true); keyInfo)
	{
		keyInfo->held = false;

		const auto out = gmpi::midi2::makeNoteOffMessage(
			keyInfo->MidiKeyNumber,
			velocity,
			keyInfo->channel
		);

		MidiIn(sampleOffset, (const unsigned char*)&out, sizeof(out));
	}
	else
	{
		// with hold pedal in Cubase, note-id may NOT be the most recent note-on for that key, but the first note-on while hold-pedal was down.
		// i.e. turn on hold pedal, play the same key repeatedly. Check for stuck notes.
		// in this case the original note-id will have been overwritten in the lookup table, so we're flying blind. But we need to avoid stuck notes.
		// !! Might need to somehow track ALL note-ids, or most-recent at least (since they are infinite)
		if (key >= 0)
		{
			const auto out = gmpi::midi2::makeNoteOffMessage(
				key,
				velocity,
				channel
			);

			MidiIn(sampleOffset, (const unsigned char*)&out, sizeof(out));
		}
	}
}

tresult Processor_VST3::setState (IBStream* state)
{
	std::string chunk;
	if (!gmpi_vst3::readSizedChunk(state, chunk))
		return kResultFalse; // short or corrupt stream: keep the current state.

	// Fail safe. This is a host boundary on the MAIN thread: the DAW calls it
	// while opening a project, and an exception escaping here unwinds into the
	// host's own event loop, where there is no handler - on macOS it reaches
	// -[NSApplication run], terminate() runs and the whole DAW aborts. A preset
	// we cannot read is a lost patch; it must never be a lost session.
	//
	// The plugin is left in its default (empty) state and the host is told the
	// restore failed, which is the honest answer.
	try
	{
		if (!active_)
		{
			plugin.setPresetUnsafe(chunk); // nothing is processing: write the store, and the next activation seeds the DSP's pins from it
		}
		else
		{
			// While active the audio thread owns the store (host controls and queued edits land in it every block), so writing it
			// here would race process(). VST3 allows setState while processing (TIDE E88). Read the preset into a scratch store and
			// send each saved value through the ui->dsp queue, the way a live edit travels: process() applies it at a block
			// boundary and updates the DSP's pins (onQueMessageReady -> sendParameterToProcessor), as the CLAP wrapper does.
			gmpi::hosting::PatchManager restored;
			restored.init(info);
			gmpi::hosting::gmpi_processor::readPreset(restored, chunk);

			// Saved values only; an empty blob is 'nothing stored yet', as start_processor's seeding treats it.
			auto toSend = [](const gmpi::hosting::GmpiParameter& param)
			{
				if (gmpi::hosting::HostControls::None != param.info->hostConnect || !param.info->is_stateful)
					return false;
				const auto* v = std::get_if<std::vector<uint8_t>>(&param.value_);
				return !(v && v->empty());
			};

			// All of it fits, or none is sent: a half-applied preset is worse than a refused one.
			int needed{};
			for (auto& [handle, param] : restored.parameters)
			{
				if (toSend(param))
					needed += param.queryQueMessageLength(0) + 64; // + frame header
			}
			if (needed > m_message_que_ui_to_dsp.freeSpace())
				return kResultFalse;

			for (auto& [handle, param] : restored.parameters)
			{
				if (!toSend(param))
					continue;

				gmpi::hosting::my_msg_que_output_stream strm(&m_message_que_ui_to_dsp); // getQueMessage writes the whole frame
				param.getQueMessage(strm, param.queryQueMessageLength(0));
				strm.Send();
			}
		}
	}
	catch (...)
	{
		return kResultFalse;
	}

	return kResultTrue;
}

// Seems to be called before audio starts to set GUI up correctly.
tresult Processor_VST3::getState (IBStream* state)
{
	// TIDE BACKLOG E68 -- PULL the state from the controller, synchronously,
	// when one is connected. The old shape serialised this processor's own
	// store, which is only as fresh as the last async IMessage the audio
	// thread had applied -- so a host that saved before the hop completed
	// (or that reads the component's state before ever calling the
	// controller's getState, which the spec allows) captured stale state.
	// Measured (TIDE E68, Ableton Live 12): a patch whose last edits were
	// its patch cables saved WITHOUT them -- modules present, wiring gone,
	// silence on reload. The controller's store is never stale, so a state
	// minted there at this instant is complete by construction: no hop, no
	// ordering, nothing to race.
	if (pairedController_)
	{
		const auto chunk = pairedController_->mintFreshPresetXml();
		int32 chunkSize = (int32)chunk.size();
		int32 bytesWritten{};
		state->write(&chunkSize, sizeof(chunkSize), &bytesWritten);
		state->write((void*)chunk.data(), chunkSize, &bytesWritten);
		return kResultTrue;
	}

	const auto chunk = plugin.getPresetUnsafe();// active_);

	int32 chunkSize = (int32) chunk.size();
	int32 bytesWritten{};

	state->write( &chunkSize, sizeof(chunkSize), &bytesWritten );
	state->write( (void*) chunk.data(), chunkSize, &bytesWritten );

	return kResultTrue;
}

tresult PLUGIN_API Processor_VST3::disconnect(Steinberg::Vst::IConnectionPoint* other)
{
	// TIDE BACKLOG E68 -- the controller may be torn down after this; a pull
	// through a freed pointer would be E66 again one layer up. getState falls
	// back to the processor's own store from here on.
	pairedController_ = {};
	return AudioEffect::disconnect(other);
}
tresult PLUGIN_API Processor_VST3::notify( IMessage* message )
{
	// WARNING: CALLED FROM GUI THREAD (In Ableton Live).
	if( !message )
		return kInvalidArgument;

	if (!strcmp(message->getMessageID(), "GmpiCtlPtr"))
	{
		// TIDE BACKLOG E68 -- the paired controller's address; see the member.
		Steinberg::int64 ptr{};
		if (message->getAttributes()->getInt("ptr", ptr) == kResultOk)
			pairedController_ = reinterpret_cast<Controller_VST3*>((intptr_t)ptr);
		return kResultOk;
	}
	if( !strcmp( message->getMessageID(), "BinaryMessage" ) )
	{
		const void* data;
		uint32 size;
		if( message->getAttributes()->getBinary( "MyData", data, size ) == kResultOk )
		{
			m_message_que_ui_to_dsp.pushString(size, (unsigned char*) data);
			m_message_que_ui_to_dsp.Send();

			return kResultOk;
		}
	}

	// SynthEdit engine feedback's reverse direction (see seMsgQues_'s declaration comment).
	// SynthRuntime::connectPeer already polls ControllerToProcessorQue() itself every block
	// (SynthRuntime.cpp), so pushing bytes in here is the entire job - nothing else reads them.
	if (seMsgQues_ && !strcmp(message->getMessageID(), "SeBinaryMessage"))
	{
		const void* data;
		uint32 size;
		if (message->getAttributes()->getBinary("MyData", data, size) == kResultOk)
		{
			auto* toDsp = seMsgQues_->ControllerToProcessorQue();
			toDsp->pushString(size, (unsigned char*)data);
			toDsp->Send();

			return kResultOk;
		}
	}

	return AudioEffect::notify( message );
}

void Processor_VST3::onQueDataAvailable()
{
	// TODO!!! This is INCORRECT. The que write pointer must be updated under the lock, else waiting thread can miss it.
	std::unique_lock<std::mutex> lk(backgroundMutex);
	backgroundSignal.notify_one();
}

// background thread
void Processor_VST3::CommunicationProc()
{
//	auto que = synthEditProject.MessageQueToGuiRaw();

	while(!killBackgroundthread)
	{
		auto readyBytes = m_message_que_dsp_to_ui.readyBytes();
		while(readyBytes > 0) // handle case of fifo wrap-arround
		{
			IMessage* message = allocateMessage();
			if(!message)
			{
				break;
			}

			Steinberg::FReleaser msgReleaser(message);
			message->setMessageID("BinaryMessage");

			void* data{};
			const auto size = m_message_que_dsp_to_ui.siphon2(&data);
			assert(size > 0);

			message->getAttributes()->setBinary("MyData", data, size);
			sendMessage(message);

			m_message_que_dsp_to_ui.siphon2_advance(size);

			readyBytes -= size;
		}

		// SynthEdit engine feedback, on its own VST3 message id (see seMsgQues_'s declaration
		// comment for why this can't share "BinaryMessage" with gmpiController's own traffic).
		if (seDspToUiQueue_)
		{
			auto seReadyBytes = seDspToUiQueue_->readyBytes();
			while (seReadyBytes > 0)
			{
				IMessage* message = allocateMessage();
				if (!message)
					break;

				Steinberg::FReleaser msgReleaser(message);
				message->setMessageID("SeBinaryMessage");

				void* data{};
				const auto size = seDspToUiQueue_->siphon2(&data);
				assert(size > 0);

				message->getAttributes()->setBinary("MyData", data, size);
				sendMessage(message);

				seDspToUiQueue_->siphon2_advance(size);

				seReadyBytes -= size;
			}
		}

		if (latencyChangePending_.exchange(false, std::memory_order_acq_rel))
		{
			if (IMessage* message = allocateMessage(); message)
			{
				Steinberg::FReleaser msgReleaser(message);
				message->setMessageID("GmpiLatency");
				message->getAttributes()->setInt("samples", plugin.getLatency());
				sendMessage(message);
			}
		}

		// wait to be signalled.
		{
			std::unique_lock<std::mutex> lk(backgroundMutex);
			backgroundSignal.wait(lk);
		}
	}
}

// MIDI from the plugin's MIDI output pins, during process().
void Processor_VST3::onMidiOut(int32_t port, int32_t timestamp, std::span<const uint8_t> bytes)
{
	if (!dataptr || !dataptr->outputEvents || dataptr->numSamples <= 0 || bytes.empty())
		return;

	midiOutBus_ = port;
	const auto offset = std::clamp(timestamp, 0, dataptr->numSamples - 1);

	// MIDI 1.0 SysEx, passed through whole. The event only points at the bytes, so keep a copy until the host has read it.
	if (bytes[0] == 0xF0)
	{
		if (sysexOutUsed_ + bytes.size() > sysexOut_.size())
			return;

		auto dest = sysexOut_.data() + sysexOutUsed_;
		std::copy(bytes.begin(), bytes.end(), dest);
		sysexOutUsed_ += bytes.size();

		Event event{};
		event.busIndex = port;
		event.sampleOffset = offset;
		event.type = Event::kDataEvent;
		event.data.type = DataEvent::kMidiSysEx;
		event.data.bytes = dest;
		event.data.size = static_cast<uint32>(bytes.size());
		dataptr->outputEvents->addEvent(event);
		return;
	}

	midiConverter.processMidi({ bytes.data(), bytes.size() }, offset);
}

}
