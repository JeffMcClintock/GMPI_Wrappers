#include "StandaloneHost.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace gmpi
{
namespace standalone
{

// ---------------------------------------------------------------------------
// MidiFifo
// ---------------------------------------------------------------------------
// Layout is [1 byte length][payload] repeated. Indices are free-running
// 32-bit counters masked into the buffer, so wrap-around needs no special
// case beyond splitting a message that straddles the end.

void MidiFifo::push(const uint8_t* data, int size)
{
    if (size <= 0 || !data)
        return;

    // Dropped whole, not clipped to fit. Clipping a sysex loses its 0xF7, and
    // the plugin has no way to tell the difference between a dump that ended and
    // a dump that stopped - see the note on the class.
    //
    // The drivers cannot produce one this long (MidiStreamParser caps them at
    // the same number) and the command channel refuses one before it gets here,
    // so this is the backstop rather than the check anybody trips: it is what
    // makes the FIFO safe against whoever pushes next. A drop is SILENT, which
    // is exactly why the command channel does its own - a caller that asked for
    // something impossible deserves to be told so rather than to read "ok" (see
    // sendMidi in mcp/CommandDispatcher.cpp).
    if (size > kMaxMessage)
        return;

    const auto write = writePos_.load(std::memory_order_relaxed);
    const auto read  = readPos_.load(std::memory_order_acquire);

    const uint32_t used = write - read;
    const uint32_t need = static_cast<uint32_t>(size) + 1;

    if (used + need > static_cast<uint32_t>(kCapacity))
        return; // full: drop this message, see the header note

    buffer_[write % kCapacity] = static_cast<uint8_t>(size);
    for (int i = 0; i < size; ++i)
        buffer_[(write + 1 + i) % kCapacity] = data[i];

    writePos_.store(write + need, std::memory_order_release);
}

int MidiFifo::pop(uint8_t* out)
{
    const auto read  = readPos_.load(std::memory_order_relaxed);
    const auto write = writePos_.load(std::memory_order_acquire);

    if (read == write)
        return 0;

    const int size = buffer_[read % kCapacity];
    for (int i = 0; i < size; ++i)
        out[i] = buffer_[(read + 1 + i) % kCapacity];

    readPos_.store(read + static_cast<uint32_t>(size) + 1, std::memory_order_release);
    return size;
}

// ---------------------------------------------------------------------------
// StandaloneHost
// ---------------------------------------------------------------------------

StandaloneHost::StandaloneHost() :
      messageQueUiToDsp_(0x500000)  // 5MB, matching the other wrappers
    , midiConverter_([this](const gmpi::midi2::message_view msg, int sampleOffset)
        {
            // Runs on the audio thread, from inside processAudio, so pushing
            // straight into the processor's event list is safe.
            if (processor_.MidiInputPinIdx < 0)
                return;

            gmpi::api::Event e
            {
                {},                                   // next (linked up by EventQue::head)
                sampleOffset,                         // timeDelta
                gmpi::api::EventType::Midi,
                processor_.MidiInputPinIdx,           // pinIdx
                static_cast<int32_t>(msg.size()),     // size_
                {}                                    // data_
            };

            // Event::data_ is an 8-byte inline buffer; anything longer would
            // have to go through oversizeData_, which means owning storage the
            // audio thread cannot allocate. Converting MIDI 1.0 never produces
            // more than 8 bytes of UMP, so this only fires on a sysex8 that
            // could not have come from this input - drop rather than overrun.
            if (msg.size() > sizeof(e.data_))
                return;

            std::copy(msg.begin(), msg.begin() + msg.size(), reinterpret_cast<uint8_t*>(&e.data_));

            processor_.events.push(e);
        })
{
}

StandaloneHost::~StandaloneHost()
{
    stopTimer();
    stopMidi();
    stopAudio();

    // Sever the editor before the controller goes: unRegisterGui drops the
    // controller's pointer to it, and the editor's own destructor may still
    // call back into its host.
    if (editorParameters_)
    {
        controller_.unRegisterGui(editorParameters_.get());
        editorParameters_->setHost(nullptr);
    }
}

bool StandaloneHost::init()
{
    auto& factory = gmpi::hosting::factory::getInstance();

    info_ = factory.getPluginInfo();
    if (!info_)
        return false;

    // Audio pin counts come from the plugin's own spec, so a mono effect opens
    // a mono device and a synth opens output-only.
    audioInCount_  = gmpi::hosting::countPins(*info_, gmpi::PinDirection::In,  gmpi::PinDatatype::Audio);
    audioOutCount_ = gmpi::hosting::countPins(*info_, gmpi::PinDirection::Out, gmpi::PinDatatype::Audio);
    hasMidiInPin_  = gmpi::hosting::countPins(*info_, gmpi::PinDirection::In,  gmpi::PinDatatype::Midi) > 0;

    controller_.init(*info_);
    processor_.init(*info_);

    // The editor's route back to the processor. Same wiring as
    // Controller_CLAP: a changed parameter joins the waiting list, and
    // onTimer feeds the list into the UI->DSP queue.
    controller_.notifyDaw = [this](gmpi::hosting::GmpiParameter* param)
        {
            pendingQueueClients_.AddWaiter(param);
        };

    // Instantiate the editor now rather than when the window opens: the window
    // size is whatever the editor asks for, so it has to exist first.
    if (auto pluginUnknown = factory.createInstance(info_->id.c_str(), gmpi::api::PluginSubtype::Editor); pluginUnknown)
    {
        editorGraphics_   = pluginUnknown.as<gmpi::api::IDrawingClient>();
        editorParameters_ = pluginUnknown.as<gmpi::api::IEditor>();
    }

    // ~60Hz, matching the other wrappers. Drives both parameter queues.
    startTimer(15);

    return true;
}

gmpi::api::IUnknown* StandaloneHost::parameterHost()
{
    return static_cast<gmpi::api::IEditorHost*>(&controller_);
}

void StandaloneHost::onEditorAttached()
{
    if (editorParameters_)
    {
        editorParameters_->initialize();
        controller_.initUi(editorParameters_.get());
    }
}

void StandaloneHost::getEditorSize(float& width, float& height)
{
    // A plugin with no GUI still needs a window - a standalone with no window
    // has no menu bar and so no way to choose a device.
    constexpr float defaultSize = 400.0f;

    // Nothing may ask the compositor for a window this large. A stretchy
    // editor answers measure() with whatever it was offered, so asking what it
    // would like at 99999 gets 99999 back - and a 99999x100025 window is not a
    // big window, it is a 40GB shm buffer and a protocol error that kills the
    // client on the spot ("Invalid width, height or stride").
    constexpr float sanityMax = 4096.0f;

    width = height = defaultSize;

    if (!editorGraphics_)
        return;

    // Two measure() calls bracket what the editor can do. The one at ZERO
    // available space is the interesting one: that is the editor's natural
    // size, because a client that wants a specific size returns it whatever it
    // is offered, and one that stretches returns its minimum. The measure at
    // maximum is only used as a ceiling.
    const gmpi::drawing::Size availableMin{ 0.f, 0.f };
    const gmpi::drawing::Size availableMax{ sanityMax, sanityMax };

    gmpi::drawing::Size naturalSize{};
    gmpi::drawing::Size maximumSize{};

    editorGraphics_->measure(&availableMin, &naturalSize);
    editorGraphics_->measure(&availableMax, &maximumSize);

    // A plugin that ignores measure() leaves these zero; treat "asked for
    // nothing" as "no opinion" rather than as a zero-sized window.
    if (maximumSize.width  <= 0.f) maximumSize.width  = defaultSize;
    if (maximumSize.height <= 0.f) maximumSize.height = defaultSize;
    if (naturalSize.width  <= 0.f) naturalSize.width  = defaultSize;
    if (naturalSize.height <= 0.f) naturalSize.height = defaultSize;

    width  = std::clamp(naturalSize.width,  1.0f, (std::min)(maximumSize.width,  sanityMax));
    height = std::clamp(naturalSize.height, 1.0f, (std::min)(maximumSize.height, sanityMax));
}

void StandaloneHost::setAudioDriver(std::unique_ptr<AudioDriver> driver)
{
    stopAudio();
    audioDriver_ = std::move(driver);
}

void StandaloneHost::setMidiDriver(std::unique_ptr<MidiDriver> driver)
{
    stopMidi();
    midiDriver_ = std::move(driver);
}

void StandaloneHost::restartProcessor(float sampleRate, int blockSize)
{
    sampleRate_ = sampleRate;
    blockSize_  = blockSize;

    // Silent input buffers, used when the plugin has audio inputs but the
    // driver opened none (no capture device, or the user picked an output-only
    // device). Without them the plugin's input pins would point at nothing.
    silenceIn_.assign(static_cast<size_t>(audioInCount_), std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
    silenceInPtr_.clear();
    for (auto& buffer : silenceIn_)
        silenceInPtr_.push_back(buffer.data());

    processorLive_ = false;
    processorLive_ = processor_.start_processor(&processor_, *info_, blockSize, sampleRate);
}

bool StandaloneHost::startAudio(const std::string& deviceId, int sampleRate, int bufferFrames)
{
    lastError_.clear();

    if (!audioDriver_)
    {
        lastError_ = "No audio driver was built into this application.";
        return false;
    }

    stopAudio();

    if (!audioDriver_->open(deviceId, sampleRate, bufferFrames, audioInCount_, audioOutCount_, this))
    {
        lastError_ = audioDriver_->lastError();
        if (lastError_.empty())
            lastError_ = "The audio device could not be opened.";
        return false;
    }

    // What the driver GRANTED, not what was asked for. A server-side graph
    // picks its own rate and quantum, and a plugin started against the
    // requested numbers would run at the wrong pitch.
    restartProcessor(
        static_cast<float>(audioDriver_->getSampleRate()),
        audioDriver_->getBufferFrames());

    if (!processorLive_)
    {
        audioDriver_->close();
        lastError_ = "The plugin's audio processor failed to start.";
        return false;
    }

    audioRunning_ = true;
    return true;
}

void StandaloneHost::stopAudio()
{
    if (audioDriver_)
        audioDriver_->close();   // returns only once any in-flight callback is done

    audioRunning_  = false;
    processorLive_ = false;
}

bool StandaloneHost::startMidi(const MidiInputSelection& inputs)
{
    // First, so that no reading of this outlives the attempt that produced it -
    // the settings pane re-reads it every time the page is shown, and a keyboard
    // plugged in since would otherwise still be accused of being absent.
    midiError_.clear();

    if (!midiDriver_ || !hasMidiInPin_)
        return false;

    stopMidi();

    // The one place the two readings of an empty id list are told apart. The
    // driver has no way to express "none" - it would open every readable input
    // instead - so a selection of nothing is answered by not opening it at all,
    // and the stopMidi() above is what makes that take effect on a driver that
    // was already running.
    //
    // True: the selection was applied. Nothing is listening because nothing is
    // what was asked for, which is not the same answer as a driver that refused
    // to open the inputs someone did choose.
    if (inputs.isNone())
        return true;

    // Into midiError_, NOT lastError_: that one is audio's, and this call runs
    // after startAudio, so writing there would replace the reason the app is
    // silent with the reason it has no keyboard.
    if (!midiDriver_->open(inputs.driverIds(), this))
    {
        midiError_ = midiDriver_->lastError();
        if (midiError_.empty())
            midiError_ = "The MIDI inputs could not be opened.";   // as startAudio does above
        return false;
    }

    midiRunning_ = true;
    return true;
}

void StandaloneHost::stopMidi()
{
    if (midiDriver_)
        midiDriver_->close();

    midiRunning_ = false;
}

void StandaloneHost::onAudioFormatChanged(float sampleRate, int maxBlockSize)
{
    restartProcessor(sampleRate, maxBlockSize);
}

void StandaloneHost::onMidiIn(const uint8_t* data, int size)
{
    midiFifo_.push(data, size);
}

void StandaloneHost::processAudio(
    int frames,
    const float* const* inputs, int inChannels,
    float* const* outputs, int outChannels)
{
    if (!processorLive_ || !processor_.processor)
    {
        for (int ch = 0; ch < outChannels; ++ch)
            std::memset(outputs[ch], 0, sizeof(float) * static_cast<size_t>(frames));
        return;
    }

    // Parameter changes from the editor. Applied before the block, so a knob
    // move lands on a block boundary - sample-accurate automation needs a
    // timeline, which a standalone does not have.
    messageQueUiToDsp_.pollMessage(&processor_);

    // MIDI collected since the last block. All stamped at offset 0: the
    // driver's timestamps are on a different clock from the audio callback's,
    // and guessing an offset would add jitter rather than remove it.
    {
        uint8_t message[MidiFifo::kMaxMessage];
        while (const int size = midiFifo_.pop(message))
            midiConverter_.processMidi({ message, static_cast<size_t>(size) }, 0);
    }

    // Point the plugin's audio pins at the driver's buffers. Pin order within
    // each direction is the pin order in the spec, which is what the device's
    // channel order means for a standalone.
    {
        int inIdx = 0;
        int outIdx = 0;

        for (const auto& pin : info_->dspPins)
        {
            if (pin.datatype != gmpi::PinDatatype::Audio)
                continue;

            if (pin.direction == gmpi::PinDirection::In)
            {
                // Fall back to silence once the device's channels run out,
                // rather than reusing channel 0 - a stereo effect fed a
                // duplicated left channel sounds subtly wrong and looks fine.
                const float* src = (inputs && inIdx < inChannels)
                                 ? inputs[inIdx]
                                 : (static_cast<size_t>(inIdx) < silenceInPtr_.size() ? silenceInPtr_[inIdx] : nullptr);

                if (src)
                    processor_.processor->setBuffer(pin.id, const_cast<float*>(src));

                ++inIdx;
            }
            else
            {
                if (outputs && outIdx < outChannels)
                    processor_.processor->setBuffer(pin.id, outputs[outIdx]);

                ++outIdx;
            }
        }
    }

    processor_.processor->process(frames, processor_.events.head());
    processor_.events.clear();

    // Anything the processor wants the editor to know (meters, output pins).
    processor_.pendingControllerQueueClients.ServiceWaitersIncremental(
        &controller_.message_que_dsp_to_ui, frames);
}

bool StandaloneHost::onTimer()
{
    // Processor -> editor.
    controller_.message_que_dsp_to_ui.pollMessage(&controller_);

    // Editor -> processor.
    pendingQueueClients_.ServiceWaitersIncremental(&messageQueUiToDsp_, 100000);

    return true;
}

} // namespace standalone
} // namespace gmpi
