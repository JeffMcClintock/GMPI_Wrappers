#pragma once

// ---------------------------------------------------------------------------
// The Wayland IPlugView (VST3 3.8.0 and later).
//
// 3.8.0 added kPlatformTypeWaylandSurfaceID and the two interfaces that make it
// work - IWaylandHost and IWaylandFrame, in
// pluginterfaces/gui/iwaylandframe.h. Before that the only Linux embedding was
// X11, which is what SEVSTGUIEditorLinux still does and remains the fallback
// for hosts that offer no Wayland (and for XWayland sessions).
//
// The shape of it:
//
//   * the plugin does NOT connect to the system compositor. The host acts as a
//     compositor for its plugins; IWaylandHost::openWaylandConnection() returns
//     a wl_display connected to it. IWaylandHost itself comes from
//     IHostApplication::createInstance, and IHostApplication arrives via
//     IPluginFactory3::setHostContext - which is why the factory keeps it.
//
//   * attached() receives the host frame's wl_surface. We create our own and
//     give it the wl_subsurface role with that as parent.
//
//   * as on X11, the host owns the event loop. Same Linux::IRunLoop contract:
//     register the connection fd and a timer, never poll ourselves.
// ---------------------------------------------------------------------------

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iwaylandframe.h"
#include "base/source/fobject.h"
#include "backends/DrawingFrameWayland.h"
#include "GmpiSdkCommon.h"
#include "GmpiApiEditor.h"
#include "VST3EditorBase.h"

namespace wrapper
{

class SEVSTGUIEditorWayland : public VST3EditorBase,
                              public Steinberg::Linux::IEventHandler,
                              public Steinberg::Linux::ITimerHandler
{
public:
    SEVSTGUIEditorWayland(gmpi::hosting::pluginInfo const& info,
                          gmpi::shared_ptr<gmpi::api::IEditor>& peditor,
                          wrapper::Controller_VST3* controller, int width, int height);
    ~SEVSTGUIEditorWayland();

    // Can this host do Wayland at all? Checked before the view is created, so
    // the controller can fall back to the X11 editor instead of handing the DAW
    // a view that will refuse every platform type it offers.
    static bool hostSupportsWayland();

    //---from IPlugView-------
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API removed() SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API onWheel(float) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16, Steinberg::int16, Steinberg::int16) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16, Steinberg::int16, Steinberg::int16) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool) SMTG_OVERRIDE { return Steinberg::kResultFalse; }
    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API canResize() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) SMTG_OVERRIDE;

    //---from Linux::IEventHandler-------
    void PLUGIN_API onFDIsSet(Steinberg::Linux::FileDescriptor fd) SMTG_OVERRIDE;

    //---from Linux::ITimerHandler-------
    void PLUGIN_API onTimer() SMTG_OVERRIDE;

    //---Interface------
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return VST3EditorBase::addRef(); }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE { return VST3EditorBase::release(); }

private:
    // Ask the host's IPlugFrame for the xdg_surface our popups must anchor to.
    // Deferred until attach: getParentSurface reports the parent's position
    // relative to OUR surface, so it is only meaningful once we have one.
    void resolvePopupParent();

    gmpi::wayland::Connection connection;
    gmpi::wayland::WaylandSubsurfaceFrame drawingframe{ connection };

    Steinberg::IPtr<Steinberg::Linux::IRunLoop> runLoop;
    Steinberg::IPtr<Steinberg::IWaylandHost>    waylandHost;
    Steinberg::IPtr<Steinberg::IWaylandFrame>   waylandFrame;

    wl_display* display{};
    bool registeredWithRunLoop = false;
    bool connectionOpen = false;
    float Dpi{ 1.0f };
};

}
