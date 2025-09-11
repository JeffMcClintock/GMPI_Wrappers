/*
 * ClapSawDemo is Free and Open Source released under the MIT license
 *
 * Copright (c) 2021, Paul Walker
 */

#include "Processor_CLAP.h"
#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>

// Eject the core symbols for the plugin
#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>
#include <clap/helpers/host-proxy.hh>
#include <clap/helpers/host-proxy.hxx>
#include <iomanip>
#include <locale>

namespace gmpi { namespace hosting
{

Processor_CLAP::Processor_CLAP(const clap_plugin_descriptor* desc, gmpi::hosting::pluginInfo& pinfo, const clap_host *host)
    : clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                            clap::helpers::CheckingLevel::Maximal>(desc, host)
    , info(pinfo)
    , midiConverter([this](const gmpi::midi::message_view msg, int sampleOffset)
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
        })
{
    plugin.init(info);
}

Processor_CLAP::~Processor_CLAP()
{
#if HAS_GUI
    // I *think* this is a bitwig bug that they won't call guiDestroy if destroying a plugin
    // with an open window but
    if (editor)
        guiDestroy();
#endif
}

/*
 * PARAMETER SETUP SECTION
 */
bool Processor_CLAP::isValidParamId(clap_id paramId) const noexcept
{
    for(const auto param : plugin.nativeParams)
        if(param->info->dawTag == paramId)
			return true;

	return false;
}

bool Processor_CLAP::paramsInfo(uint32_t paramIndex, clap_param_info *clap_info) const noexcept
{
    if (paramIndex < 0 || paramIndex >= plugin.nativeParams.size())
        return false;

	const auto& param_info = *plugin.nativeParams[paramIndex]->info;

    *clap_info = {};

    /*
     * Our job is to populate the clap_param_info. We set each of our parameters as AUTOMATABLE
     * and then begin setting per-parameter features.
     */
    clap_info->flags = CLAP_PARAM_IS_AUTOMATABLE;

    /*
     * These constants activate polyphonic modulatability on a parameter. Not all the params here
     * support that
     */
   // TODO auto mod = CLAP_PARAM_IS_MODULATABLE; // | CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY;

    strncpy(clap_info->name, param_info.name.c_str(), CLAP_NAME_SIZE);
    strncpy(clap_info->module, "", CLAP_NAME_SIZE);
    clap_info->id            = param_info.dawTag;
    clap_info->min_value     = param_info.minimum;
    clap_info->max_value     = param_info.maximum;
    clap_info->default_value = param_info.default_value;

    if ((param_info.datatype == gmpi::PinDatatype::Int32 || param_info.datatype == gmpi::PinDatatype::Int64) && !param_info.enum_entries.empty())
    {
        clap_info->flags |= CLAP_PARAM_IS_STEPPED;
        //clap_info->stepCount = (std::max)(0, static_cast<int>(p.info->enum_entries.size()) - 1);
    }

    return true;
}

bool Processor_CLAP::paramsValue(clap_id paramId, double* value) noexcept
{
    assert(paramId >= 0 && paramId < plugin.nativeParams.size());

    if (paramId < 0 || paramId >= plugin.nativeParams.size())
        return false;

    auto p = plugin.nativeParams[paramId];

    *value = p->valueReal;

    return true;
}

bool Processor_CLAP::paramsValueToText(clap_id paramId, double value, char *display,
                                    uint32_t size) noexcept
{
    assert(paramId >= 0 && paramId < plugin.nativeParams.size());

    if (paramId < 0 || paramId >= plugin.nativeParams.size())
        return false;

    const auto& param = *plugin.nativeParams[paramId];

    std::string valueString;

    // enums
    if ((param.info->datatype == gmpi::PinDatatype::Int32 || param.info->datatype == gmpi::PinDatatype::Int64) && !param.info->enum_entries.empty())
    {
        const int index = std::clamp(static_cast<int>(std::round(value)), 0, static_cast<int>(param.info->enum_entries.size()) - 1);
		valueString = param.info->enum_entries[index].name;
    }
    else
    {
        valueString = std::to_string(value);
	}

    strncpy(display, valueString.c_str(), size);
    display[size - 1] = '\0';

#if 0
    auto pid = (paramIds)paramId;
    std::string sValue{"ERROR"};
    auto n2s = [](auto n)
    {
        std::ostringstream oss;
        oss << std::setprecision(6) << n;
        return oss.str();
    };
    switch (pid)
    {
    case pmResonance:
    case pmPreFilterVCA:
        sValue = n2s(value);
        break;
    case pmAmpRelease:
    case pmAmpAttack:
        sValue = n2s(scaleTimeParamToSeconds(value)) + " s";
        break;
    case pmUnisonCount:
    {
        int vc = static_cast<int>(value);
        sValue = n2s(vc) + (vc == 1 ? " voice" : " voices");
        break;
    }
    case pmUnisonSpread:
    case pmOscDetune:
        sValue = n2s(value) + " cents";
        break;
    case pmAmpIsGate:
        sValue = value > 0.5 ? "AEG Bypassed" : "AEG On";
        break;
    case pmCutoff:
    {
        auto co = 440 * pow(2.0, (value - 69) / 12);
        sValue = n2s(co) + " Hz";
        break;
    }
    case pmFilterMode:
    {
        auto fm = (SawDemoVoice::StereoSimperSVF::Mode) static_cast<int>(value);
        switch (fm)
        {
        case SawDemoVoice::StereoSimperSVF::LP:
            sValue = "LowPass";
            break;
        case SawDemoVoice::StereoSimperSVF::BP:
            sValue = "BandPass";
            break;
        case SawDemoVoice::StereoSimperSVF::HP:
            sValue = "HighPass";
            break;
        case SawDemoVoice::StereoSimperSVF::NOTCH:
            sValue = "Notch";
            break;
        case SawDemoVoice::StereoSimperSVF::PEAK:
            sValue = "Peak";
            break;
        case SawDemoVoice::StereoSimperSVF::ALL:
            sValue = "AllPass";
            break;
        }
        break;
    }
    }

    strncpy(display, sValue.c_str(), size);
    display[size - 1] = '\0';
#endif

    return true;
}

bool Processor_CLAP::paramsTextToValue(clap_id paramId, const char *display, double *value) noexcept
{
    assert(paramId >= 0 && paramId < plugin.nativeParams.size());

    if (paramId < 0 || paramId >= plugin.nativeParams.size())
        return false;

    const auto& param = *plugin.nativeParams[paramId];

    *value = std::clamp(std::atof(display), param.info->minimum, param.info->maximum);

    // todo enums

    return true;
#if 0
    switch (paramId)
    {
    case pmResonance:
    case pmPreFilterVCA:
        *value = std::clamp(std::atof(display), 0., 1.);
        return true;
        break;
    case pmAmpRelease:
    case pmAmpAttack:
        *value = scaleSecondsToTimeParam(std::atof(display));
        return true;
        break;
    case pmUnisonCount:
    {
        *value = std::clamp(std::atoi(display), 1, 7);
        return true;
        break;
    }
    case pmUnisonSpread:
        *value = std::clamp(std::atof(display), 0., 100.);
        return true;
        break;

    case pmOscDetune:
        *value = std::clamp(std::atof(display), -200.0, 200.0);
        return true;
        break;
    case pmCutoff:
    {
        // auto co = 440 * pow(2.0, (value - 69) / 12);
        // log2(co/440) = (value - 69)/12
        // value = log2(co/440) * 12 + 69

        auto cohz = std::clamp(std::atof(display), 1.0, 25000.0);
        *value = log2(cohz / 440.0) * 12 + 69;
        return true;
        break;
    }
        // Skip these two. You get the idea
    case pmFilterMode:
    case pmAmpIsGate:
        return false;
        break;
    }
#endif
    return false;
}

/*
 * Stereo out, Midi in, in a pretty obvious way.
 * The only trick is the idi in also has NOTE_DIALECT_CLAP which provides us
 * with options on note expression and the like.
 */
bool Processor_CLAP::audioPortsInfo(uint32_t index, bool isInput,
                                 clap_audio_port_info *info) const noexcept
{
    if (isInput || index != 0)
        return false;

    info->id = 0;
    info->in_place_pair = CLAP_INVALID_ID;
    strncpy(info->name, "main", sizeof(info->name));
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    return true;
}

bool Processor_CLAP::notePortsInfo(uint32_t index, bool isInput,
                                clap_note_port_info *info) const noexcept
{
    if (isInput)
    {
        info->id = 1;
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI2;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI2;
        strncpy(info->name, "NoteInput", CLAP_NAME_SIZE);
        return true;
    }
    return false;
}

bool Processor_CLAP::activate(double psampleRate, uint32_t minFrameCount,
    uint32_t pmaxFrameCount) noexcept
{
    sampleRate = psampleRate;
    maxFrameCount = pmaxFrameCount;

    plugin.start_processor(this, info);

    return true;
}

/*
 * The process function is the heart of any CLAP. It reads inbound events,
 * generates audio if appropriate, writes outbound events, and informs the host
 * to continue operating.
 *
 * In the ClapSawDemo, our process loop has 3 basic stages
 *
 * 1. See if the UI has sent us any events on the thread-safe UI Queue (
 *    see the discussion in the clap header file for this structure), apply them
 *    to my internal state, and generate CLAP changed messages
 *
 * 2. Iterate over samples rendering the voices, and if an inbound event is coincident
 *    with a sample, process that event for note on, modulation, parameter automation, and so on
 *
 * 3. Detect any voices which have terminated in the block (their state has become 'NEWLY_OFF'),
 *    update them to 'OFF' and send a CLAP NOTE_END event to terminate any polyphonic modulators.
 */
clap_process_status Processor_CLAP::process(const clap_process *process) noexcept
{
    // If I have no outputs, do nothing
    if (process->audio_outputs_count <= 0)
        return CLAP_PROCESS_SLEEP;

    auto& plugin_ = plugin.processor;
    auto& events = plugin.events;


    /*
     * Stage 1:
     *
     * The UI can send us gesture begin/end events which translate in to a
     * `clap_event_param_gesture` or value adjustments. Handle those.
     */
//    handleEventsFromUIQueue(process->out_events);

    {
        auto ev = process->in_events;
        auto sz = ev->size(ev);

        for (int i = 0; i < sz; ++i)
        {
            if (auto evt = ev->get(ev, i); evt)
            {
                switch (evt->type)
                {
                case CLAP_EVENT_MIDI:
                {
                    auto mevt = reinterpret_cast<const clap_event_midi*>(evt);
					const int size = gmpi::midi_1_0::status_type::ChannelPressure == (mevt->data[0] & 0xF0) ? 2 : 3;
                    midiConverter.processMidi({ mevt->data, size }, evt->time);
                }
                break;

                case CLAP_EVENT_MIDI2:
                {
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

                    auto mevt = reinterpret_cast<const clap_event_midi2*>(evt);
                    const auto src = reinterpret_cast<const uint8_t*>(mevt->data);

                    const auto message_type = src[3] >> 4;
                    const auto message_length = midi_message_size[message_type];

                    if (message_length < 3)
                    {
                        uint8_t reversebuffer[8];

                        int bufferIndex = 0;
                        // 32-bit messages.
                        reversebuffer[bufferIndex++] = src[3];
                        reversebuffer[bufferIndex++] = src[2];
                        reversebuffer[bufferIndex++] = src[1];
                        reversebuffer[bufferIndex++] = src[0];

                        if (message_length == 2)
                        {
                            // 64-bit messages
                            reversebuffer[bufferIndex++] = src[7];
                            reversebuffer[bufferIndex++] = src[6];
                            reversebuffer[bufferIndex++] = src[5];
                            reversebuffer[bufferIndex++] = src[4];
                        }

                        midiConverter.processMidi(
                            { reversebuffer, message_length * 4 }
                            , static_cast<int>(evt->time)
                        );
                    }
                }
                break;

                case CLAP_EVENT_PARAM_VALUE:
                {
                    auto v = reinterpret_cast<const clap_event_param_value*>(evt);
					const auto& inID = v->param_id;

                    //*paramToValue[v->param_id] = v->value;
                    //pushParamsToVoices();

                    if (inID >= 0 && inID < plugin.nativeParams.size())
                    {
                        auto p = plugin.nativeParams[inID];

                        plugin.setParameterNormalizedFromDaw(
                             *plugin.info
                            , evt->time
                            , p->info->id
                            , plugin.nativeParams[inID]->real2Normalized(v->value)
                        );
                    }
#if HAS_GUI
                    if (editor)
                    {
                        auto r = ToUI();
                        r.type = ToUI::PARAM_VALUE;
                        r.id = v->param_id;
                        r.value = (double)v->value;

                        toUiQ.try_enqueue(r);
                    }
#endif
                }

                default:
                    break;
                }
            }
        }
    }

#if HAS_GUI
    /*
     * and then update transport information for the display on our
     * shared state object
     */
    if (process->transport)
    {
        dataCopyForUI.tempo = process->transport->tempo;
        dataCopyForUI.tsDen = process->transport->tsig_denom;
        dataCopyForUI.tsNum = process->transport->tsig_num;
        dataCopyForUI.songpos = 1.0 * process->transport->song_pos_beats / CLAP_BEATTIME_FACTOR;
    }
#endif

    /*
     * Stage 2: Create the AUDIO output and process events
     *
     * CLAP has a single inbound event loop where every event is time stamped with
     * a sample id. This means the process loop can easily interleave note and parameter
     * and other events with audio generation. Here we do everything completely sample accurately
     * by maintaining a pointer to the 'nextEvent' which we check at every sample.
     */
    float** inputBuffers = process->audio_inputs ? process->audio_inputs[0].data32 : nullptr;
    float** outputBuffers = process->audio_outputs ? process->audio_outputs[0].data32 : nullptr;
	auto chansIn = process->audio_inputs? process->audio_inputs->channel_count : 0;
    auto chansOut = process->audio_outputs ? process->audio_outputs->channel_count : 0;

    // pass buffer pointers to plugin
    {
        int inIdx = 0;
        int outIdx = 0;
        for (auto& pin : info.dspPins)
        {
            if (pin.datatype != gmpi::PinDatatype::Audio)
                continue;

            if (pin.direction == gmpi::PinDirection::In)
            {
                plugin_->setBuffer(pin.id, process->audio_inputs[0].data32[inIdx++]);
            }
            else
            {
                plugin_->setBuffer(pin.id, process->audio_outputs[0].data32[outIdx++]);
            }
        }

        assert(inIdx == chansIn);
        assert(outIdx == chansOut);
    }

    // Process audio.
    plugin_->process(process->frames_count, events.head());

    events.clear();

    return CLAP_PROCESS_CONTINUE;

#if 0
    // This pointer is the sentinel to our next event which we advance once an event is processed
    const clap_event_header_t *nextEvent{nullptr};
    uint32_t nextEventIndex{0};
    if (sz != 0)
    {
        nextEvent = ev->get(ev, nextEventIndex);
    }

    for (int i = 0; i < process->frames_count; ++i)
    {
        // Do I have an event to process. Note that multiple events
        // can occur on the same sample, hence 'while' not 'if'
        while (nextEvent && nextEvent->time == i)
        {
            // handleInboundEvent is a separate function which adjusts the state based
            // on event type. We segregate it for clarity but you really should read it!
            handleInboundEvent(nextEvent);
            nextEventIndex++;
            if (nextEventIndex >= sz)
                nextEvent = nullptr;
            else
                nextEvent = ev->get(ev, nextEventIndex);
        }

        // This is a simple accumulator of output across our active voices.
        // See saw-voice.h for information on the individual voice.
        for (int ch = 0; ch < chansOut; ++ch)
        {
            out[ch][i] = 0.f;
        }
        for (auto &v : voices)
        {
            if (v.isPlaying())
            {
                v.step();
                if (chansOut >= 2)
                {
                    out[0][i] += v.L;
                    out[1][i] += v.R;
                }
                else if (chansOut == 1)
                {
                    out[0][i] += (v.L + v.R) * 0.5;
                }
            }
        }
    }

    /*
     * Stage 3 is to inform the host of our terminated voices.
     *
     * This allows hosts which support polyphonic modulation to terminate those
     * modulators, and it is also the reason we have the NEWLY_OFF state in addition
     * to the OFF state.
     *
     * Note that there are two ways to enter the terminatedVoices array. The first
     * is here through natural state transition to NEWLY_OFF and the second is in
     * handleNoteOn when we steal a voice.
     */
    for (auto &v : voices)
    {
        if (v.state == SawDemoVoice::NEWLY_OFF)
        {
            terminatedVoices.emplace_back(v.portid, v.channel, v.key, v.note_id);
            v.state = SawDemoVoice::OFF;
        }
    }

    for (const auto &[portid, channel, key, note_id] : terminatedVoices)
    {
        auto ov = process->out_events;
        auto evt = clap_event_note();
        evt.header.size = sizeof(clap_event_note);
        evt.header.type = (uint16_t)CLAP_EVENT_NOTE_END;
        evt.header.time = process->frames_count - 1;
        evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        evt.header.flags = 0;

        evt.port_index = portid;
        evt.channel = channel;
        evt.key = key;
        evt.note_id = note_id;
        evt.velocity = 0.0;

        ov->try_push(ov, &(evt.header));

#if HAS_GUI
        dataCopyForUI.updateCount++;
        dataCopyForUI.polyphony--;
#endif
    }
    terminatedVoices.clear();

    // We should have gotten all the events
    assert(!nextEvent);

    // A little optimization - if we have any active voices continue
    for (const auto &v : voices)
    {
        if (v.state != SawDemoVoice::OFF)
        {
            return CLAP_PROCESS_CONTINUE;
        }
    }

    // Otherwise we have no voices - we can return CLAP_PROCESS_SLEEP until we get the next event
    // And our host can optionally skip processing
    return CLAP_PROCESS_SLEEP;
#endif
}

void Processor_CLAP::handleEventsFromUIQueue(const clap_output_events_t *ov)
{
#if HAS_GUI
    bool uiAdjustedValues{false};
    ClapSawDemo::FromUI r;
    while (fromUiQ.try_dequeue(r))
    {
        switch (r.type)
        {
        case FromUI::BEGIN_EDIT:
        case FromUI::END_EDIT:
        {
            auto evt = clap_event_param_gesture();
            evt.header.size = sizeof(clap_event_param_gesture);
            evt.header.type = (r.type == FromUI::BEGIN_EDIT ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                            : CLAP_EVENT_PARAM_GESTURE_END);
            evt.header.time = 0;
            evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            evt.header.flags = 0;
            evt.param_id = r.id;
            ov->try_push(ov, &evt.header);

            break;
        }
        case FromUI::ADJUST_VALUE:
        {
            // So set my value
            *paramToValue[r.id] = r.value;

            // But we also need to generate outbound message to the host
            auto evt = clap_event_param_value();
            evt.header.size = sizeof(clap_event_param_value);
            evt.header.type = (uint16_t)CLAP_EVENT_PARAM_VALUE;
            evt.header.time = 0; // for now
            evt.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            evt.header.flags = 0;
            evt.param_id = r.id;
            evt.value = r.value;

            ov->try_push(ov, &(evt.header));

            uiAdjustedValues = true;
        }
        }
    }

    // Similarly we need to push values to a UI on startup
    if (refreshUIValues && editor)
    {
        _DBGCOUT << "Pushing a refresh of UI values to the editor" << std::endl;
        refreshUIValues = false;

        for (const auto &[k, v] : paramToValue)
        {
            auto r = ToUI();
            r.type = ToUI::PARAM_VALUE;
            r.id = k;
            r.value = *v;
            toUiQ.try_enqueue(r);
        }
    }

    if (uiAdjustedValues)
        pushParamsToVoices();
#endif
}


/*
 * If the processing loop isn't running, the call to requestParamFlush from the UI will
 * result in this being called on the main thread, and generating all the appropriate
 * param updates.
 */
void Processor_CLAP::paramsFlush(const clap_input_events *in, const clap_output_events *out) noexcept
{
    auto sz = in->size(in);

    // This pointer is the sentinel to our next event which we advance once an event is processed
    for (auto e = 0U; e < sz; ++e)
    {
        auto nextEvent = in->get(in, e);
// TODO        handleInboundEvent(nextEvent);
    }

    handleEventsFromUIQueue(out);

    // We will never generate a note end event with processing active, and we have no midi
    // output, so we are done.
}

bool Processor_CLAP::stateSave(const clap_ostream *stream) noexcept
{
#if 0 // TODO
    // Oh this is soooo bad. Please don't judge me. I'm just trying to get this
    // together for launch day! If you are using this as an example for your plugins,
    // you should write a less dumb serializer of course. On and I bet this might have
    // a locale problem?
    std::ostringstream oss;
    auto cloc = std::locale("C");
    oss.imbue(cloc);
    oss << "STREAM-VERSION-1;";
    for (const auto &[id, val] : paramToValue)
    {
        oss << id << "=" << std::setw(30) << std::setprecision(20) << *val << ";";
    }
//    _DBGCOUT << oss.str() << std::endl;

    auto st = oss.str();
    auto c = st.c_str();
    auto s = st.length() + 1; // write the null terminator
    while (s > 0)
    {
        auto r = stream->write(stream, c, s);
        if (r < 0)
            return false;
        s -= r;
        c += r;
    }

#endif

    return true;
}

bool Processor_CLAP::stateLoad(const clap_istream *stream) noexcept
{
#if 0 // TODO
    // Again, see the comment above on 'this is terrible'
    static constexpr uint32_t maxSize = 4096 * 8, chunkSize = 256;
    char buffer[maxSize];
    char *bp = &(buffer[0]);
    int64_t rd{0};
    int64_t totalRd{0};

    buffer[0] = 0;
    while ((rd = stream->read(stream, bp, chunkSize)) > 0)
    {
        bp += rd;
        totalRd += rd;
        if (totalRd >= maxSize - chunkSize - 1)
        {
 //           _DBGCOUT << "Invalid stream: Why did you send me so many bytes!" << std::endl;
            // What the heck? You sdent me more than 32kb of data for a 700 byte string?
            // That means my next chunk read will blow out memory so....
            return false;
        }
    }

    // Make sure I'm null terminated in case you hand me total garbage
    if (totalRd < maxSize)
        buffer[totalRd] = 0;

    auto dat = std::string(buffer);
//    _DBGCOUT << dat << std::endl;

    std::vector<std::string> items;
    size_t spos{0};
    while ((spos = dat.find(';')) != std::string::npos)
    {
        auto l = dat.substr(0, spos);
        dat = dat.substr(spos + 1);
        items.push_back(l);
    }

    if (items[0] != "STREAM-VERSION-1")
    {
//        _DBGCOUT << "Invalid stream" << std::endl;
        return false;
    }
    for (auto i : items)
    {
        auto epos = i.find('=');
        if (epos == std::string::npos)
            continue; // oh well
        auto id = std::atoi(i.substr(0, epos).c_str());
        double val = 0.0;
        std::istringstream istr(i.substr(epos + 1));
        istr.imbue(std::locale("C"));
        istr >> val;

        *(paramToValue[(paramIds)id]) = val;
    }

    pushParamsToVoices();
#endif

    return true;
}

/*
 * A simple passthrough. Put it here to allow the template mechanics to see the impl.
 */
void Processor_CLAP::editorParamsFlush()
{
    if (_host.canUseParams())
        _host.paramsRequestFlush();
}

#if IS_LINUX && HAS_GUI
bool ClapSawDemo::registerTimer(uint32_t interv, clap_id *id)
{
    auto res = _host.timerSupportRegister(interv, id);
    return res;
}
bool ClapSawDemo::unregisterTimer(clap_id id) { return _host.timerSupportUnregister(id); }
bool ClapSawDemo::registerPosixFd(int fd)
{
    return _host.posixFdSupportRegister(fd, CLAP_POSIX_FD_READ | CLAP_POSIX_FD_WRITE |
                                                CLAP_POSIX_FD_ERROR);
}
bool ClapSawDemo::unregisterPosixFD(int fd) { return _host.posixFdSupportUnregister(fd); }
#endif

}} // namespace
