#pragma once

// ---------------------------------------------------------------------------
// The Linux IPlugView.
//
// Two things make this different from the Windows and macOS editors, and both
// come from the VST3 specification rather than from us:
//
//  * The only Linux embedding VST3 defines is kPlatformTypeX11EmbedWindowID -
//    an X11 Window id. There is NO Wayland platform type in 3.7.14 or earlier,
//    so a Wayland-native host reaches its plugins through XWayland. See
//    docs/vst3-linux-editor.md for what a Wayland extension would need.
//
//  * A Linux plugin may not run an event loop. The host owns it, and lends the
//    plugin a Steinberg::Linux::IRunLoop (queried off IPlugFrame) to register a
//    file descriptor and a timer with. Everything this class does is a reaction
//    to one of those two callbacks.
// ---------------------------------------------------------------------------

#include "pluginterfaces/gui/iplugview.h"
#include "base/source/fobject.h"
#include "backends/DrawingFrameX11.h"
#include "GmpiSdkCommon.h"
#include "GmpiApiEditor.h"
#include "VST3EditorBase.h"

namespace wrapper
{

class SEVSTGUIEditorLinux : public VST3EditorBase,
                            public Steinberg::Linux::IEventHandler,
                            public Steinberg::Linux::ITimerHandler
{
    gmpi::hosting::X11DrawingFrame drawingframe;
    Steinberg::IPtr<Steinberg::Linux::IRunLoop> runLoop;
    bool registeredWithRunLoop = false;
    float Dpi{ 1.0f };

public:
    SEVSTGUIEditorLinux(gmpi::hosting::pluginInfo const& info,
                        gmpi::shared_ptr<gmpi::api::IEditor>& peditor,
                        wrapper::Controller_VST3* controller, int width, int height);
    ~SEVSTGUIEditorLinux();

    //---from IPlugView-------
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API removed() SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API onWheel(float /*distance*/) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 /*key*/, Steinberg::int16 /*keyMsg*/,
                                            Steinberg::int16 /*modifiers*/) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 /*key*/, Steinberg::int16 /*keyMsg*/,
                                          Steinberg::int16 /*modifiers*/) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool /*state*/) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API canResize() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) SMTG_OVERRIDE;

    //---from Linux::IEventHandler-------
    void PLUGIN_API onFDIsSet(Steinberg::Linux::FileDescriptor fd) SMTG_OVERRIDE;

    //---from Linux::ITimerHandler-------
    void PLUGIN_API onTimer() SMTG_OVERRIDE;

    //---Interface------
    // Hand-written rather than the DEF_INTERFACE macros: the host queries this
    // one object for three unrelated interfaces, and FObject's macro form only
    // knows how to build one chain.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return VST3EditorBase::addRef(); }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE { return VST3EditorBase::release(); }
};

}
