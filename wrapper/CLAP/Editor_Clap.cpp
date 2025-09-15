#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"
#include "backends/DrawingFrameWin.h"
#endif

#include "./Factory_Clap.h"
#include "Hosting/gmpi_factory.h"
#include "Editor_CLAP.h"
#include "Processor_CLAP.h"

#if __APPLE__

// without including objective-C headers, we need to create an NSView.
// forward declare function here to return the view, using void* as return type.
void* createNativeView(void* parent, class IUnknown* paramHost, class IUnknown* client, int width, int height);
void gmpi_onCloseNativeView(void* ptr);

#endif

namespace gmpi {
namespace hosting
{

/*
 * Part one of this implementation is the plugin adapters which allow a GUI to attach.
 * These are the Processor_CLAP methods. First up: Which windowing API do we support.
 * Pretty obviously, mac supports cocoa, windows supports win32, and linux supports
 * X11.
 */
bool Processor_CLAP::guiIsApiSupported(const char* api, bool isFloating) noexcept
{
    if (isFloating)
        return false;

#if __APPLE__
    if (strcmp(api, CLAP_WINDOW_API_COCOA) == 0)
        return true;
#endif

#ifdef _WIN32
    if (strcmp(api, CLAP_WINDOW_API_WIN32) == 0)
        return true;
#endif

    /*
#if IS_LINUX
    if (_host.canUseTimerSupport() && _host.canUsePosixFdSupport() &&
        strcmp(api, CLAP_WINDOW_API_X11) == 0)
        return true;
#endif
    */

    return false;
}

/*
 * GUICreate gets called when the host requests the plugin create its editor with
 * a given API. We ignore the API and isFloating here, because we handled them
 * above and assume our host follows the protocol that it only calls us with
 * values which are supported.
 *
 * The important thing from a VSTGUI perspective here is that we have to initialize
 * the VSTGUI static data structures. On Mac and Windows, this is an easy call and
 * we can use the VSTGUI::finally mechanism to clean up. On Linux there is a more
 * complicated global event loop to merge which, thanks to the way VSTGUI structures
 * their event loops, is a touch more awkward. As such the linux code is all in a different
 * cpp file for individual documentation (Please see the README for any linux disclaimers
 * and most recent status).
 */
bool Processor_CLAP::guiCreate(const char* api, bool isFloating) noexcept
{
    static bool everInit{ false };
    if (!everInit)
    {
#if IS_MAC
        VSTGUI::init(CFBundleGetMainBundle());
#endif
#if IS_WIN
        VSTGUI::init(GetModuleHandle(nullptr));
#endif

        // This proves unreliable
        //static auto cleanup = VSTGUI::finally(
        //    []()
        //    {
        //        //_DBGCOUT << "Exiting VSTGUI" << std::endl;
        //        //VSTGUI::exit();
        //        //_DBGCOUT << "VSTGUI Exit done" << std::endl;
        //    });

        everInit = true;
    }

    editor = new Editor_CLAP(&controller.gmpiController); // toUiQ, fromUiQ, dataCopyForUI, [this]() { editorParamsFlush(); });

    return editor != nullptr;
}

/*
 * guiDestroy destroys the editor object and returns it to the
 * nullptr sentinel, to stop ::process sending events to the ui.
 */
void Processor_CLAP::guiDestroy() noexcept
{
    // We need to split this because of linux
//    editor->haltIdleTimer();

#if !IS_LINUX
    // Oh linux is still giving me lifecycle problems... get back to this
//    editor->getFrame()->close();
#endif

#if IS_LINUX
    removeLinuxVSTGUIPlugin(this);
#endif

    if (editor)
        delete editor;
    editor = nullptr;
}

/*
 * guiSetParent is the core API for a clap HOST which has a window to
 * reparent the editor to that host managed window. It sends a
 * `const clap_window *window` data structure which contains a union of
 * platform specific window pointers.
 *
 * VSTGUI handles reparenting through `VSTGUI::CFrame::open` which consumes
 * a pointer to a native window. This makes adapting easy. Our editor object
 * owns a `CFrame` as its base window, and setParent opens it with the new
 * parent platform specific item handed to it.
 */
bool Processor_CLAP::guiSetParent(const clap_window* window) noexcept
{
#if __APPLE__
    editor->open(window->cocoa);
#endif
    //#if IS_LINUX
    //    editor->open((void*)(window->x11));
    //#endif
#ifdef _WIN32
    editor->open(window->win32);
#endif


    // Once we are reparented, we can set up our UI
    //editor->setupUI();

    //if (dataCopyForUI.isProcessing)
    //{
    //    // and ask the engine to refresh from the processing thread
    //    refreshUIValues = true;
    //}
    //else
    //{
    //    // Pull the parameters on the main thread
    //    for (const auto& [k, v] : paramToValue)
    //    {
    //        auto r = ToUI();
    //        r.type = ToUI::PARAM_VALUE;
    //        r.id = k;
    //        r.value = *v;
    //        toUiQ.try_enqueue(r);
    //    }
    //}
    // And we are done!
    return true;
}

#ifdef _WIN32
LRESULT CALLBACK Editor_CLAPWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    auto drawingFrame = (gmpi::hosting::DxDrawingFrameBase*)(LONG_PTR)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (drawingFrame)
    {
        return drawingFrame->WindowProc(hwnd, message, wParam, lParam);
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}
#endif

Editor_CLAP::Editor_CLAP(
    gmpi_controller_holder* pgmpiController

    /*Processor_CLAP::SynthToUI_Queue_t& i,
    Processor_CLAP::UIToSynth_Queue_t& o,
    const Processor_CLAP::DataCopyForUI& d, std::function<void()> pf)
    : inbound(i), outbound(o), synthData(d), paramRequestFlush(std::move(pf)) */)
    : gmpiController(pgmpiController)
{
    
#ifdef _WIN32
    drawingframe.setFallbackHost(static_cast<gmpi::api::IEditorHost*>(gmpiController));
#endif
    
    // instansiate client now, so it can be measured.
    if (auto info = gmpi::hosting::factory::getInstance().getPluginInfo(); info)
    {
        auto pluginUnknown = gmpi::hosting::factory::getInstance().createInstance(info->id.c_str(), gmpi::api::PluginSubtype::Editor);
        pluginGraphics_GMPI = pluginUnknown.as<gmpi::api::IDrawingClient>();
        pluginParameters_GMPI = pluginUnknown.as<gmpi::api::IEditor>();
    }
    
#ifdef _WIN32
    if (pluginParameters_GMPI)
    {
        pluginParameters_GMPI->setHost(static_cast<gmpi::api::IDrawingHost*>(&drawingframe));
    }
#endif
    
#if __APPLE__
#endif
}

Editor_CLAP::~Editor_CLAP()
{
	if (pluginParameters_GMPI)
		gmpiController->unRegisterGui(pluginParameters_GMPI.get());
    
#if __APPLE__
    gmpi_onCloseNativeView(nsView);
#endif
}

void Editor_CLAP::getSize(uint32_t& width, uint32_t& height)
{
    
#ifdef _WIN32
    // DPI of system. only a GUESS at this point of DPI we will be using. (until we know DAW window handle).
    {
        HDC hdc = ::GetDC(NULL);
        Dpi = GetDeviceCaps(hdc, LOGPIXELSX) / 96.f;
        ::ReleaseDC(NULL, hdc);
    }
#endif
    
    if (pluginGraphics_GMPI)
    {
        constexpr float defaultSize = 100.0f;

        gmpi::drawing::Size minimumSize{ 0.f, 0.f };
        gmpi::drawing::Size maximumSize{ 0.f, 0.f };
        gmpi::drawing::Size availableSizeMin{ 0.f, 0.f };
        gmpi::drawing::Size availableSizeMax{ 99999.f, 99999.f };

        pluginGraphics_GMPI->measure(&availableSizeMin, &minimumSize);
        pluginGraphics_GMPI->measure(&availableSizeMax, &maximumSize);

        gmpi::drawing::Size finalSize{ defaultSize, defaultSize };

		finalSize.width = std::clamp(finalSize.width, minimumSize.width, maximumSize.width);
		finalSize.height = std::clamp(finalSize.height, minimumSize.height, maximumSize.height);

        finalSize.width = (std::max)(1.0f, finalSize.width);
        finalSize.height = (std::max)(1.0f, finalSize.height);

        width = static_cast<uint32_t>(Dpi * finalSize.width);
        height = static_cast<uint32_t>(Dpi * finalSize.height);
    }
    else
    {
        width = 32;
        height = 32;
    }

#if 0


    if (!drawingClient)
    {
        width = 0;
        height = 0;
        return;
    }

    gmpi::drawing::Size desiredSize{ 100.f, 100.f };
    gmpi::drawing::Size availableSize{ 99999.f, 99999.f };
    drawingClient->measure(&availableSize, &desiredSize);

    // DPI of system. only a GUESS at this point of DPI we will be using. (until we know DAW window handle).
    {
        HDC hdc = ::GetDC(NULL);
        Dpi = GetDeviceCaps(hdc, LOGPIXELSX) / 96.f;
        ::ReleaseDC(NULL, hdc);
    }

    width = static_cast<uint32_t>(Dpi * desiredSize.width);
    height = static_cast<uint32_t>(Dpi * desiredSize.height);
#endif
}

void Editor_CLAP::setSize(uint32_t pwidth, uint32_t pheight)
{
    width = pwidth;
    height = pheight;

    gmpi::drawing::Rect r{ 0.f, 0.f, width / Dpi, height / Dpi };

    if (pluginGraphics_GMPI)
        pluginGraphics_GMPI->arrange(&r);
}

void Editor_CLAP::open(void* parentWindow)
{
#if 0 // TODO
    clientInvalidated = [this]()
        {
            detachAndRecreate();

            auto info = gmpi::hosting::factory::getInstance().getPluginInfo();

            if (!info)
                return;

            auto pluginUnknown = gmpi::hosting::factory::getInstance().createInstance(info->id.c_str(), gmpi::api::PluginSubtype::Editor);

            if (!pluginUnknown)
                return;

            auto editor = pluginUnknown.as<gmpi::api::IEditor>();

            if (!editor)
                return;

            attachClient(editor.get());

            gmpi::drawing::Rect r{ 0.f, 0.f, width / Dpi, height / Dpi };
            drawingClient->arrange(&r);
        };
#endif

#ifdef _WIN32

    // now that we know which monitor we're on, update Dpi.
    Dpi = GetDpiForWindow((HWND)parentWindow) / 96.f;

    if (pluginGraphics_GMPI)
    {
        drawingframe.attachClient(pluginGraphics_GMPI.get());

        const gmpi::drawing::SizeL overrideSize{ static_cast<int32_t>(width), static_cast<int32_t>(height) };
        drawingframe.open(parentWindow, &overrideSize);

        //        controller->gmpiController.initUi(&helper);
    }

    if (pluginParameters_GMPI)
    {
        pluginParameters_GMPI->initialize();
    }
#endif
    
#if __APPLE__
    nsView = createNativeView(
          parentWindow
          , (class IUnknown*) static_cast<gmpi::api::IEditorHost*>(gmpiController)
          , (class IUnknown*) pluginParameters_GMPI.get()
          , width, height
          );
#endif
    
    if (pluginParameters_GMPI)
    {
        gmpiController->initUi(pluginParameters_GMPI.get());
    }

#if  0 // def _WIN32
    // while constructing editor, JUCE main window is a small fixed size, so no point querying it. easier to just pass in required size.
    RECT r{ 0, 0, width, height };

    auto dllhandle = (HINSTANCE) gmpi::hosting::tempSharedD2DBase::getDllHandle();
    const auto windowClass = gmpi::hosting::RegisterWindowsClass(dllhandle, Editor_CLAPWindowProc);
    myhwnd = gmpi::hosting::CreateHostingWindow(dllhandle, windowClass, (HWND)parentWindow, r, (LONG_PTR)static_cast<gmpi::hosting::DxDrawingFrameBase*>(this));

    if (!myhwnd)
        return;

    CreateSwapPanel(DrawingFactory.getD2dFactory());

    initTooltip();

//    clientInvalidated();
    if (drawingClient)
    {
        attachClient(drawingClient.get());

        const auto scale = 1.0 / getRasterizationScale();

        sizeClientDips(
            static_cast<float>(width) * scale,
            static_cast<float>(height) * scale);

        //gmpi::drawing::Rect r{ 0.f, 0.f, width / Dpi, height / Dpi };
        //drawingClient->arrange(&r);
    }
#endif
}


/*
 * guiSetScale is the core API that allows the Host to set the absolute GUI
 * scaling factor, and override any OS info. This is important to allow the UI
 * to correctly reflect what has been specified by the Host and not have to
 * work out the users intentions through some sort of magic.
 *
 * Obviously, the value will depend on how the host chooses to implement it.
 * The value is normalised, with 1.0 representing 100% scaling.
 */
bool Processor_CLAP::guiSetScale(double scale) noexcept
{
    assert(editor);
 //   _DBGCOUT << _D(scale) << std::endl;
//    editor->setUIScale(scale);
    return true;
}

/*
 * Sizing is described in the gui extension, but this implementation
 * means that if the host drags to resize, we accept its size and resize our frame
 */
bool Processor_CLAP::guiSetSize(uint32_t width, uint32_t height) noexcept
{
    assert(editor);
 //   _DBGCOUT << _D(width) << _D(height) << std::endl;
    //editor->getFrame()->setSize(width, height);
    //editor->resize();
    //editor->getFrame()->invalid();

    editor->setSize(width, height);
    return true;
}

/*
 * Returns the size of the UI window, presumable so a host can better layout plugin UIs
 * if grouped together.
 */
bool Processor_CLAP::guiGetSize(uint32_t* width, uint32_t* height) noexcept
{
    assert(editor);
	editor->getSize(*width, *height);
    //*width = editor->applyUIScale(GUI_DEFAULT_W);
    //*height = editor->applyUIScale(GUI_DEFAULT_H);
    return true;
}

bool Processor_CLAP::guiAdjustSize(uint32_t* width, uint32_t* height) noexcept
{
    assert(editor);
    // If I wanted to I could apply a constraint here, but I choose not to.
    return true;
}


#if 0
// Create and add our UI objects with a callback tag. Completely standard VSTGUI
void Editor_CLAP::setupUI()
{
    auto scaleFont = [this](VSTGUI::CFontRef font)
        {
            auto res = VSTGUI::makeOwned<VSTGUI::CFontDesc>(*font);
            res->setSize(res->getSize() * uiScale);
            res->remember();
            return res;
        };
    knF = scaleFont(VSTGUI::kNormalFont);
    knFVeryBig = scaleFont(VSTGUI::kNormalFontVeryBig);
    knFSmall = scaleFont(VSTGUI::kNormalFontSmall);
    knFSmaller = scaleFont(VSTGUI::kNormalFontSmaller);

    // Resize as we should now have our scale
    frame->setSize(applyUIScale(Processor_CLAP::GUI_DEFAULT_W),
        applyUIScale(Processor_CLAP::GUI_DEFAULT_H));
    frame->invalid();

    backgroundRender = new ClapSawDemoBackground(
        VSTGUI::CRect(0, 0, getFrame()->getWidth(), getFrame()->getHeight()));
    backgroundRender->uiScale = uiScale;
    frame->addView(backgroundRender);

    auto l = new VSTGUI::CTextLabel(VSTGUI::CRect(0, 0, getFrame()->getWidth(), applyUIScale(25)),
        "Clap Saw Synth Demo");
    l->setTransparency(true);
    l->setFont(knFVeryBig);
    l->setHoriAlign(VSTGUI::CHoriTxtAlign::kCenterText);
    topLabel = l;
    frame->addView(topLabel);

    l = new VSTGUI::CTextLabel(
        VSTGUI::CRect(VSTGUI::CPoint(0, applyUIScale(40)),
            VSTGUI::CPoint(getFrame()->getWidth(), applyUIScale(20))),
        "poly=0");
    l->setTransparency(true);
    l->setFont(knFSmall);
    l->setHoriAlign(VSTGUI::CHoriTxtAlign::kCenterText);
    statusLabel = l;
    frame->addView(statusLabel);

    l = new VSTGUI::CTextLabel(
        VSTGUI::CRect(VSTGUI::CPoint(0, applyUIScale(27)),
            VSTGUI::CPoint(getFrame()->getWidth(), applyUIScale(20))),
        "transport=0");
    l->setTransparency(true);
    l->setFont(knFSmall);
    l->setHoriAlign(VSTGUI::CHoriTxtAlign::kCenterText);
    transportLabel = l;
    frame->addView(transportLabel);

    l = new VSTGUI::CTextLabel(
        VSTGUI::CRect(VSTGUI::CPoint(0, getFrame()->getHeight() - applyUIScale(40)),
            VSTGUI::CPoint(getFrame()->getWidth(), applyUIScale(20))),
        "https://github.com/surge-synthesizer/clap-saw-demo");
    l->setTransparency(true);
    l->setFont(knFSmall);
    l->setHoriAlign(VSTGUI::CHoriTxtAlign::kCenterText);
    repoLabel = l;
    frame->addView(repoLabel);

    auto sl = std::string("MIT License; CLAP v.") + std::to_string(CLAP_VERSION_MAJOR) + "." +
        std::to_string(CLAP_VERSION_MINOR) + "." + std::to_string(CLAP_VERSION_REVISION) +
        "; Built: " + __DATE__ + " @ " + __TIME__;
    l = new VSTGUI::CTextLabel(
        VSTGUI::CRect(VSTGUI::CPoint(0, getFrame()->getHeight() - applyUIScale(20)),
            VSTGUI::CPoint(getFrame()->getWidth(), applyUIScale(20))),
        sl.c_str());
    l->setTransparency(true);
    l->setFont(knFSmaller);
    l->setHoriAlign(VSTGUI::CHoriTxtAlign::kCenterText);
    bottomLabel = l;
    frame->addView(bottomLabel);

    auto mkSliderWithLabel = [this](int x, int y, int tag, const std::string& label)
        {
            auto q =
                new VSTGUI::CSlider(VSTGUI::CRect(VSTGUI::CPoint(applyUIScale(x), applyUIScale(y)),
                    VSTGUI::CPoint(applyUIScale(25), applyUIScale(150))),
                    this, tag, 0, applyUIScale(150), nullptr, nullptr);
            q->setMin(0);
            q->setMax(1);
            q->setDrawStyle(VSTGUI::CSlider::kDrawFrame | VSTGUI::CSlider::kDrawValue |
                VSTGUI::CSlider::kDrawBack);
            q->setStyle(VSTGUI::CSlider::kVertical | VSTGUI::CSlider::kBottom);
            frame->addView(q);

            auto l = new VSTGUI::CTextLabel(VSTGUI::CRect(
                VSTGUI::CPoint(applyUIScale(x) - applyUIScale(10), applyUIScale(y) + applyUIScale(155)),
                VSTGUI::CPoint(applyUIScale(45), applyUIScale(15))));
            l->setText(label.c_str());
            l->setFont(knF);

            frame->addView(l);
            return q;
        };

    auto oscRow = 70, aegRow = 70 + 150 + 80, endRow = 160;
    oscUnison = mkSliderWithLabel(10, oscRow, tags::unict, "Uni Ct");
    paramIdToCControl[Processor_CLAP::pmUnisonCount] = oscUnison;

    oscSpread = mkSliderWithLabel(70, oscRow, tags::unisp, "Spread");
    paramIdToCControl[Processor_CLAP::pmUnisonSpread] = oscSpread;

    oscDetune = mkSliderWithLabel(130, oscRow, tags::oscdetune, "Detune");
    oscDetune->setMin(-1);
    oscDetune->setDrawStyle(oscDetune->getDrawStyle() | VSTGUI::CSlider::kDrawValueFromCenter |
        VSTGUI::CSlider::kDrawInverted);
    paramIdToCControl[Processor_CLAP::pmOscDetune] = oscDetune;

    ampAttack = mkSliderWithLabel(10, aegRow, tags::env_a, "Attack");
    paramIdToCControl[Processor_CLAP::pmAmpAttack] = ampAttack;

    ampRelease = mkSliderWithLabel(70, aegRow, tags::env_r, "Release");
    paramIdToCControl[Processor_CLAP::pmAmpRelease] = ampRelease;

    preFilterVCA = mkSliderWithLabel(210, endRow, tags::vca, "VCA");
    paramIdToCControl[Processor_CLAP::pmPreFilterVCA] = preFilterVCA;

    filtCutoff = mkSliderWithLabel(290, endRow, tags::cutoff, "Cutoff");
    paramIdToCControl[Processor_CLAP::pmCutoff] = filtCutoff;

    filtRes = mkSliderWithLabel(350, endRow, tags::resonance, "Res");
    paramIdToCControl[Processor_CLAP::pmResonance] = filtRes;

    idleTimer = new VSTGUI::CVSTGUITimer([this](VSTGUI::CVSTGUITimer*) { this->idle(); }, 33);
    idleTimer->remember();
}

Editor_CLAP::~Editor_CLAP()
{
    _DBGMARK;
    frame->forget();
    frame = nullptr;
}

void Editor_CLAP::haltIdleTimer()
{
    if (idleTimer)
    {
        _DBGCOUT << "Stopping idle timer" << std::endl;
        idleTimer->stop();
        idleTimer->forget();
        idleTimer = nullptr;
    }
    else
    {
        _DBGCOUT << "No Idle Timer present; haltIdleTimer doing nothing" << std::endl;
    }
}
// We add this resize method and call it from setSize to scale the background and recenter labels
void Editor_CLAP::resize()
{
    /*
     * guiSetSize can be called before guiSetParent and guiSetParent is where we
     * create our sub-components. So be defensive here.
     */
    if (!backgroundRender)
        return;

    auto w = getFrame()->getWidth();
    auto h = getFrame()->getHeight();
    backgroundRender->setViewSize(VSTGUI::CRect(0, 0, w, h));
    backgroundRender->invalid();

    topLabel->setViewSize(VSTGUI::CRect(0, 0, w, applyUIScale(25)));
    topLabel->invalid();

    statusLabel->setViewSize(
        VSTGUI::CRect(VSTGUI::CPoint(0, applyUIScale(40)), VSTGUI::CPoint(w, applyUIScale(20))));
    statusLabel->invalid();

    transportLabel->setViewSize(
        VSTGUI::CRect(VSTGUI::CPoint(0, applyUIScale(27)), VSTGUI::CPoint(w, applyUIScale(20))));
    transportLabel->invalid();

    repoLabel->setViewSize(VSTGUI::CRect(VSTGUI::CPoint(0, h - applyUIScale(40)),
        VSTGUI::CPoint(w, applyUIScale(20))));
    repoLabel->invalid();
    bottomLabel->setViewSize(VSTGUI::CRect(VSTGUI::CPoint(0, h - applyUIScale(20)),
        VSTGUI::CPoint(w, applyUIScale(20))));
    bottomLabel->invalid();
}

/*
 * A tiny little utility mapping between our VSTGUI control tags and
 * our synth parameters. These *could* be the same but having them different
 * makes sure I don't assume they are the same.
 */
uint32_t Editor_CLAP::paramIdFromTag(int32_t tag)
{
    switch ((Editor_CLAP::tags)tag)
    {
    case tags::env_r:
        return Processor_CLAP::pmAmpRelease;
    case tags::unict:
        return Processor_CLAP::pmUnisonCount;
    case tags::unisp:
        return Processor_CLAP::pmUnisonSpread;
    case tags::env_a:
        return Processor_CLAP::pmAmpAttack;
    case tags::vca:
        return Processor_CLAP::pmPreFilterVCA;
    case tags::cutoff:
        return Processor_CLAP::pmCutoff;
    case tags::resonance:
        return Processor_CLAP::pmResonance;
    case tags::oscdetune:
        return Processor_CLAP::pmOscDetune;
    }
    assert(false);
    return 0;
}

/*
 * The primary thing valueChanged needs to do is
 *
 * 1; Scale our VSTGUI 0..1 or -1..1 values to the right scale and
 * 2: Send an outbound queue event to the lock free ui -> engine queue with
 *    the info.
 */
void Editor_CLAP::valueChanged(VSTGUI::CControl* c)
{
    auto t = (tags)c->getTag();
    auto q = Processor_CLAP::FromUI();
    q.id = paramIdFromTag(t);
    q.type = Processor_CLAP::FromUI::MType::ADJUST_VALUE;
    auto send = true;
    switch (t)
    {
        // 0..1
    case tags::resonance:
    case tags::vca:
    case tags::env_a:
    case tags::env_r:
    {
        q.value = c->getValue();
        break;
    }
    case tags::unisp:
    {
        q.value = c->getValue() * 100.0;
        break;
    }
    case tags::oscdetune:
    {
        q.value = c->getValue() * 200.0;
        break;
    }
    case tags::unict:
    {
        q.value = c->getValue() * SawDemoVoice::max_uni;
        break;
    }
    case tags::cutoff:
    {
        q.value = c->getValue() * 126 + 1;
    }
    }
    if (send)
    {
        outbound.try_enqueue(q);
        paramRequestFlush();
    }
}

/*
 * Similarly, beginEdit / endEdit need to map the gui tag to a param id and then
 * enqueue an outbound event.
 */
void Editor_CLAP::beginEdit(int32_t tag)
{
    auto q = Processor_CLAP::FromUI();
    q.id = paramIdFromTag(tag);
    q.type = Processor_CLAP::FromUI::MType::BEGIN_EDIT;
    outbound.try_enqueue(q);
    paramRequestFlush();
}
void Editor_CLAP::endEdit(int32_t tag)
{
    auto q = Processor_CLAP::FromUI();
    q.id = paramIdFromTag(tag);
    q.type = Processor_CLAP::FromUI::MType::END_EDIT;
    outbound.try_enqueue(q);
    paramRequestFlush();
}

/*
 * The ::idle method polls the inbound queue and value-based data structure,
 * responds by rescaling values and setting them on UI elements, and then invalidates
 * the appropriate UI control.
 */
void Editor_CLAP::idle()
{
    Processor_CLAP::ToUI r;
    while (inbound.try_dequeue(r))
    {
        if (r.type == Processor_CLAP::ToUI::MType::PARAM_VALUE)
        {
            auto q = paramIdToCControl.find(r.id);

            if (q != paramIdToCControl.end())
            {
                auto cc = q->second;
                auto val = r.value;

                switch (r.id)
                {
                case Processor_CLAP::pmUnisonSpread:
                    val = val / 100.0;
                    break;
                case Processor_CLAP::pmOscDetune:
                    val = val / 200.0;
                    break;
                case Processor_CLAP::pmUnisonCount:
                    val = val / SawDemoVoice::max_uni;
                    break;
                case Processor_CLAP::pmCutoff:
                    val = (val - 1) / 126.0;
                    break;

                default:
                    break;
                }
                cc->setValue(val);
                cc->invalid();
            }
        }
    }

    if (synthData.updateCount != lastDataUpdate)
    {
        lastDataUpdate = synthData.updateCount;

        auto sl = std::string("poly=") + std::to_string(synthData.polyphony);
        statusLabel->setText(sl.c_str());
        statusLabel->invalid();
        backgroundRender->polyCount = synthData.polyphony;
        backgroundRender->isProcessing = synthData.isProcessing;
        backgroundRender->invalid();
    }

    std::ostringstream oss;
    oss << "tempo=" << synthData.tempo << " ts=" << synthData.tsNum << "/" << synthData.tsDen
        << " songpos=" << std::setprecision(8) << synthData.songpos;
    transportLabel->setText(oss.str().c_str());
    transportLabel->invalid();
}
void Editor_CLAP::setUIScale(double scale)
{
    if (scale > 0)
    {
        uiScale = scale;
    }
}

// Small irrelevant detail of how we draw the background
void ClapSawDemoBackground::draw(VSTGUI::CDrawContext* dc)
{
    auto sc = [this](double i) { return double(i * uiScale); };

    auto r = VSTGUI::CRect(0, 0, getWidth(), getHeight());
    dc->setFillColor(VSTGUI::CColor(0x20, 0x20, 0x50));
    dc->drawRect(r, VSTGUI::kDrawFilled);

    auto t = VSTGUI::CRect(0, 0, getWidth(), sc(60));
    dc->setFillColor(VSTGUI::CColor(0x40, 0x40, 0x90));
    dc->drawRect(t, VSTGUI::kDrawFilled);

    auto b =
        VSTGUI::CRect(VSTGUI::CPoint(0, getHeight() - sc(40)), VSTGUI::CPoint(getWidth(), sc(40)));
    dc->setFillColor(VSTGUI::CColor(0x40, 0x40, 0x90));
    dc->drawRect(b, VSTGUI::kDrawFilled);

    if (polyCount == 0)
    {
        dc->setFrameColor(VSTGUI::CColor(0x80, 0x80, 0xA0));
        dc->setLineWidth(sc(1));
    }
    else
    {
        auto add = std::clamp(polyCount * 5, 0, 0x40);
        dc->setFrameColor(VSTGUI::CColor(0xAF + add, 0xAF + add, 0xAF + add));
        dc->setLineWidth(sc(2 + polyCount / 5.0));
    }

    dc->drawLine(VSTGUI::CPoint(sc(160), sc(90)), VSTGUI::CPoint(sc(222), sc(90)));
    dc->drawLine(VSTGUI::CPoint(sc(222), sc(90)), VSTGUI::CPoint(sc(222), sc(150)));
    dc->drawLine(VSTGUI::CPoint(sc(100), sc(400)), VSTGUI::CPoint(sc(222), sc(400)));
    dc->drawLine(VSTGUI::CPoint(sc(222), sc(400)), VSTGUI::CPoint(sc(222), sc(340)));
    dc->drawLine(VSTGUI::CPoint(sc(240), sc(235)), VSTGUI::CPoint(sc(285), sc(235)));

    if (isProcessing)
    {
        dc->setFillColor(VSTGUI::CColor(0x60, 0xA0, 0x60));
        dc->setFrameColor(VSTGUI::CColor(0xAF, 0xFF, 0xAF));
    }
    else
    {
        dc->setFillColor(VSTGUI::CColor(0xA0, 0x60, 0x60));
        dc->setFrameColor(VSTGUI::CColor(0xFF, 0xAF, 0xAF));
    }
    dc->setLineWidth(sc(1));
    dc->drawEllipse(VSTGUI::CRect(VSTGUI::CPoint(sc(10), sc(10)), VSTGUI::CPoint(sc(15), sc(15))),
        VSTGUI::kDrawFilledAndStroked);
}
#endif

}
}
