#pragma once

#include <iostream>
//#include "debug-helpers.h"

/*
 * ClapSawDemo is the core synthesizer class. It uses the clap-helpers C++ plugin extensions
 * to present the CLAP C API as a C++ object model.
 *
 * The core features here are
 *
 * - Hold the CLAP description static object
 * - Advertise parameters and ports
 * - Provide an event handler which responds to events and returns sound
 * - Do voice management. Which is really not very sophisticated (it's just an array of 64
 *   voice objects and we choose the next free one, and if you ask for a 65th voice, nothing
 *   happens).
 * - Provide the API points to delegate UI creation to a separate editor object,
 *   coded in clap-saw-demo-editor
 *
 * This demo is coded to be relatively familiar and close to programming styles form other
 * formats where the editor and synth collaborate closely; as described in clap-saw-demo-editor
 * this object also holds the two queues the editor and synth use to communicate; and holds the
 * bundle of atomic values to which the editor holds a const &.
 */

#include "clap/helpers/plugin.hh"
#include <atomic>
#include <array>
#include <unordered_map>
#include <memory>
#include "Hosting/processor_holder.h"
#include "Hosting/controller_holder.h"
#include "GmpiMidi.h"

namespace gmpi {
namespace hosting
{

struct Processor_CLAP : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
    clap::helpers::CheckingLevel::Maximal>
    , public gmpi::api::IProcessorHost
{
    gmpi::hosting::gmpi_processor plugin;
    gmpi::hosting::gmpi_controller_holder gmpiController;

    gmpi::hosting::pluginInfo& info;
    gmpi::midi_2_0::MidiConverter2 midiConverter;

    static constexpr int max_voices = 64;
    Processor_CLAP(const clap_plugin_descriptor* desc, gmpi::hosting::pluginInfo& info, const clap_host* host);
    ~Processor_CLAP();

    /*
     * Activate makes sure sampleRate is distributed through
     * the data structures, in this case by stamping the sampleRate
     * onto each pre-allocated voice object.
     */
    bool activate(double sampleRate, uint32_t minFrameCount,
        uint32_t maxFrameCount) noexcept override;

    /*
     * Parameter Handling:
     *
     * Each parameter gets a unique ID which is returned by 'paramsInfo' when the plugin
     * is instantiated (or the plugin asks the host to reset). To avoid accidental bugs where
     * I confuse creation index with param IDs, I am using arbitrary numbers for each
     * parameter id.
     *
     * The implementation of paramsInfo contains the setup of these params.
     *
     * The actual synth has a very simple model to update parameter values. It
     * contains a map from these IDs to a double * which the constructor sets up
     * as references to members.
     */
#if 0
    enum paramIds : uint32_t
    {
        pmUnisonCount = 1378,
        pmUnisonSpread = 2391,
        pmOscDetune = 8675309,

        pmAmpAttack = 2874,
        pmAmpRelease = 728,
        pmAmpIsGate = 1942,

        pmPreFilterVCA = 87612,

        pmCutoff = 17,
        pmResonance = 94,
        pmFilterMode = 14255
    };
    static constexpr int nParams = 10;
#endif
    bool implementsParams() const noexcept override { return true; }
    bool isValidParamId(clap_id paramId) const noexcept override;
    uint32_t paramsCount() const noexcept override { return plugin.nativeParams.size(); }
    bool paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept override;
    bool paramsValue(clap_id paramId, double* value) noexcept override;

    /*
     * This converts the numerical value of the parameter to a display value for the DAW.
     * For instance we model filter cutoff in 12-TET MIDI Note space, so the value
     * "60" of pmCutoff shows as "261.6 hz" and "69" (concert A) as "440 hz". Similarly
     * this is where we show our time scaling for our attack and release, filter type,
     * and so on. If you want, you can also implement paramsTextToValue which is the
     * inverse function, for hosts which allow user typeins. In this example, we choose
     * to not implement that.
     */
    bool paramsValueToText(clap_id paramId, double value, char* display,
        uint32_t size) noexcept override;

protected:
    bool paramsTextToValue(clap_id paramId, const char* display, double* value) noexcept override;

public:
    // Convert 0-1 linear into 0-4s exponential
    float scaleTimeParamToSeconds(float param);
    float scaleSecondsToTimeParam(float seconds);

    /*
     * Many CLAP plugins will want input and output audio and note ports, although
     * the spec doesn't require this. Here as a simple synth we set up a single s
     * stereo output and a single midi / clap_note input.
     */
    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0 : 1; }
    bool audioPortsInfo(uint32_t index, bool isInput,
        clap_audio_port_info* info) const noexcept override;

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool isInput) const noexcept override { return isInput ? 1 : 0; }
    bool notePortsInfo(uint32_t index, bool isInput,
        clap_note_port_info* info) const noexcept override;

    /*
     * VoiceInfo is an optional (currently draft) extension where you advertise
     * polyphony information. Crucially here though it allows you to advertise that
     * you can support overlapping notes, which in conjunction with CLAP_DIALECT_NOTE
     * and the Bitwig voice stack modulator lets you stack this little puppy!
     */
    bool implementsVoiceInfo() const noexcept override { return false; }
    bool voiceInfoGet(clap_voice_info* info) noexcept override
    {
        info->voice_capacity = max_voices;
        info->voice_count = max_voices;
        info->flags = CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES;
        return true;
    }

    /*
     * I have an unacceptably crude state dump and restore. If you want to
     * improve it, PRs welcome! But it's just like any other read-and-write-goop
     * from-a-stream api really.
     */
    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream*) noexcept override;
    bool stateLoad(const clap_istream*) noexcept override;

    /*
     * process is the meat of the operation. It does obvious things like trigger
     * voices but also handles all the polyphonic modulation and so on. Please see the
     * comments in the cpp file to understand it and the helper functions we have
     * delegated to.
     */
    clap_process_status process(const clap_process* process) noexcept override;
    void handleEventsFromUIQueue(const clap_output_events_t*);

    /*
     * In addition to ::process, the plugin should implement ::paramsFlush. ::paramsFlush will be
     * called when processing isn't active (no audio being generated, etc...) but the host or UI
     * wants to update a value - usually a parameter value. In effect it looks like a version
     * of process with no audio buffers.
     */
    void paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept override;

    /*
     * start and stop processing are called when you start and stop obviously.
     * We update an atomic bool so our UI can go ahead and draw processing state
     * and also flush param changes when there is no processing queue.
     */
    bool startProcessing() noexcept override
    {
#if HAS_GUI
        dataCopyForUI.isProcessing = true;
        dataCopyForUI.updateCount++;
#endif
        return true;
    }
    void stopProcessing() noexcept override
    {
#if HAS_GUI
        dataCopyForUI.isProcessing = false;
        dataCopyForUI.updateCount++;
#endif
    }

protected:
#if 1 //HAS_GUI
    /*
     * OK so now you see how the engine works. Great! But how does the GUI work?
     * CLAP is based on extensions and the core gui extension has a simple
     * protocol for sizing, for supported APIs, and for reparenting a component
     * with a platform-native window. Here we are going to use VSTGUI to create
     * a small UI which attaches to our CLAP. That's the API below.
     *
     * But that UI runs in another thread, and all the CLAP events are handled
     * in process, so we also need to think about inter-thread communication.
     * To do that we have three core data structures, a function, and one pointer
     *
     * - A pointer to an editor object (here a concrete editor, but a more advanced
     *   implementation could make that a proxy or a bool), which we test for null
     *   when the editor is open
     * - A lock-free queue from the engine to the UI for things like parameter
     *   updates. This queue is written in `ClapSawDemo::process` if editor is
     *   non-null and is read in the `::idle` loop of the editor on the UI thread
     * - A lock-free queue from the UI to the engine for things like begin and end
     *   gestures and value changes. This is written on the UI thread and read in
     *   stage 1 of `CLapSawDemo::process` go update engine parameters and send parameter
     *   change events to the host from the processing thread.
     * - A data structure which contains std::atomic values and where the editor keeps
     *   an in-memory const& to it. ::process updates a counter and the idle loop looks
     *   for counter changes. This allows values to propagate without events, and we use
     *   it here for polyphony count.
     * - A single std::function<void()> which the editor can use to ask the host to do
     *   a parameter flush.
     *
     * These functions are members of ClapSawDemo but we implement them in
     * `clap-saw-demo-editor.cpp` along with the VSTGUI implementation. You can consult the
     * extensive comments in the clap gui extension for semantics and rules.
     */

     // Note these are implemented in Editor_CLAP.cpp
    bool implementsGui() const noexcept override { return true; }
    bool guiIsApiSupported(const char* api, bool isFloating) noexcept override;

    bool guiCreate(const char* api, bool isFloating) noexcept override;
    void guiDestroy() noexcept override;
    bool guiSetParent(const clap_window* window) noexcept override;

    bool guiSetScale(double scale) noexcept override;
    bool guiCanResize() const noexcept override { return true; }
    bool guiAdjustSize(uint32_t* width, uint32_t* height) noexcept override;
    bool guiSetSize(uint32_t width, uint32_t height) noexcept override;
    bool guiGetSize(uint32_t* width, uint32_t* height) noexcept override;

    // Setting this atomic to true will force a push of all current engine
    // params to ui using the queue mechanism
//    std::atomic<bool> refreshUIValues{false};
#endif

    // This is an API point the editor can call back to request the host to flush
    // bound by a lambda to the editor. For a technical template reason its implemented
    // (trivially) in clap-saw-demo.cpp not demo-editor
    void editorParamsFlush();

#if IS_LINUX && HAS_GUI
    // PLEASE see the README comments on Linux. We are working on making this more rational
    // but the VSTGUI global plus runnign a bit out of time has this implementation right now
    // which can leak or crash in some circumstances when you delete a plugin. Expect updates
    // soon enough.
public:
    bool implementsTimerSupport() const noexcept override
    {
        _DBGMARK;
        return true;
    }
    void onTimer(clap_id timerId) noexcept override;

    bool registerTimer(uint32_t interv, clap_id* id);
    bool unregisterTimer(clap_id id);

    bool implementsPosixFdSupport() const noexcept override
    {
        _DBGMARK;
        return true;
    }
    void onPosixFd(int fd, clap_posix_fd_flags_t flags) noexcept override;
    bool registerPosixFd(int fd);
    bool unregisterPosixFD(int fd);

#endif

public:

    // IProcessorHost
    gmpi::ReturnCode setPin(int32_t timestamp, int32_t pinId, int32_t size, const uint8_t* data) override { return gmpi::ReturnCode::Ok; };
    gmpi::ReturnCode setPinStreaming(int32_t timestamp, int32_t pinId, bool isStreaming) override { return gmpi::ReturnCode::Ok; };
    gmpi::ReturnCode setLatency(int32_t latency) override { return gmpi::ReturnCode::Ok; };
    gmpi::ReturnCode sleep() override { return gmpi::ReturnCode::Ok; };
    int32_t getBlockSize() override { return maxFrameCount; }
    float getSampleRate() override { return sampleRate; }
    int32_t getHandle() override { return 0; };

#if HAS_GUI
    static constexpr uint32_t GUI_DEFAULT_W = 390, GUI_DEFAULT_H = 530;

    /*
     * These are the core data structures we use for the communication
     * outlined above.
     */
    struct ToUI
    {
        enum MType
        {
            PARAM_VALUE = 0x31,
            MIDI_NOTE_ON,
            MIDI_NOTE_OFF
        } type;

        uint32_t id;  // param-id for PARAM_VALUE, key for noteon/noteoff
        double value; // value or unused
    };

    struct FromUI
    {
        enum MType
        {
            BEGIN_EDIT = 0xF9,
            END_EDIT,
            ADJUST_VALUE
        } type;
        uint32_t id;
        double value;
    };

    /*
     * For some UI display information, we have a shared bundle of values which
     * we keep in the editor as a const &
     */
    struct DataCopyForUI
    {
        std::atomic<uint32_t> updateCount{ 0 };
        std::atomic<bool> isProcessing{ false };
        std::atomic<int> polyphony{ 0 };
        std::atomic<double> tempo{ 0 };
        std::atomic<int> tsNum{ 0 }, tsDen{ 0 };
        std::atomic<double> songpos{ 0 };
    } dataCopyForUI;

    typedef moodycamel::ReaderWriterQueue<ToUI, 4096> SynthToUI_Queue_t;
    typedef moodycamel::ReaderWriterQueue<FromUI, 4096> UIToSynth_Queue_t;

    SynthToUI_Queue_t toUiQ;
    UIToSynth_Queue_t fromUiQ;

private:
#endif
    struct Editor_CLAP* editor{ nullptr };

    double sampleRate{ 44100.0 };
    uint32_t maxFrameCount{ 0 };

    GMPI_QUERYINTERFACE_METHOD(gmpi::api::IProcessorHost);
    GMPI_REFCOUNT_NO_DELETE;
};
}
} // namespace
