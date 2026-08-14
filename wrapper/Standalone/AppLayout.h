#pragma once

// The window's root drawing client: a menu bar across the top, and below it
// one of several content pages (the plugin's editor, or the settings panel).
//
// Modelled on SynthEdit's SE2::TopStripLayout, trimmed to what a standalone
// needs and depending on gmpi_ui alone so this wrapper stays free of
// SynthEditLib. The part worth copying exactly is ChildHost: a child draws in
// its own (0,0)-origin space, so every rect it hands the host - invalidations,
// but also popup, text-edit and key-listener anchors - has to be offset into
// window space on the way out. Get that wrong and the plugin's right-click
// menu opens at the top of the window, over the menu bar.
//
// Pages are all constructed and hosted up front and merely switched between,
// rather than attached and detached on demand. A plugin editor is entitled to
// assume setHost is called once; re-hosting it every time someone opens the
// settings panel would be a needless way to find out which plugins agree.

#include <algorithm>
#include <memory>
#include <vector>

#include "GmpiUiDrawing.h"
#include "GmpiApiCommon.h"
#include "GmpiSdkCommon.h"
#include "helpers/NativeUi.h"

namespace gmpi
{
namespace standalone
{

class AppLayout :
      public gmpi::api::IDrawingClient
    , public gmpi::api::IInputClient
    , public gmpi::api::IGraphicsRedrawClient
{
public:
    // Per-child host: forwards to the window's host, mapping the child's
    // coordinates into window space via `offset`.
    struct ChildHost :
          public gmpi::api::IDrawingHost
        , public gmpi::api::IInputHost
        , public gmpi::api::IDialogHost
    {
        AppLayout* owner{};
        gmpi::drawing::Size offset{};

        // IDrawingHost
        gmpi::ReturnCode getDrawingFactory(gmpi::api::IUnknown** returnFactory) override
        {
            return owner->drawingHost_ ? owner->drawingHost_->getDrawingFactory(returnFactory)
                                       : gmpi::ReturnCode::Fail;
        }
        void invalidateRect(const gmpi::drawing::Rect* invalidRect) override
        {
            if (!owner->drawingHost_)
                return;

            if (!invalidRect)
            {
                owner->drawingHost_->invalidateRect(nullptr);
                return;
            }

            const auto r = gmpi::drawing::offsetRect(*invalidRect, offset);
            owner->drawingHost_->invalidateRect(&r);
        }
        void invalidateMeasure() override
        {
            if (owner->drawingHost_) owner->drawingHost_->invalidateMeasure();
        }
        float getRasterizationScale() override
        {
            return owner->drawingHost_ ? owner->drawingHost_->getRasterizationScale() : 1.0f;
        }

        // IInputHost
        gmpi::ReturnCode setCapture() override
        {
            return owner->inputHost_ ? owner->inputHost_->setCapture() : gmpi::ReturnCode::Fail;
        }
        gmpi::ReturnCode getCapture(bool& v) override
        {
            if (owner->inputHost_) return owner->inputHost_->getCapture(v);
            v = false;
            return gmpi::ReturnCode::Ok;
        }
        gmpi::ReturnCode releaseCapture() override
        {
            return owner->inputHost_ ? owner->inputHost_->releaseCapture() : gmpi::ReturnCode::Fail;
        }

        // IDialogHost - the rect-taking three need the same offset treatment
        // as invalidateRect, for the same reason.
        gmpi::ReturnCode createTextEdit(const gmpi::drawing::Rect* r, gmpi::api::IUnknown** o) override
        {
            if (!owner->dialogHost_) return gmpi::ReturnCode::NoSupport;
            const auto pr = gmpi::drawing::offsetRect(*r, offset);
            return owner->dialogHost_->createTextEdit(&pr, o);
        }
        gmpi::ReturnCode createPopupMenu(const gmpi::drawing::Rect* r, gmpi::api::IUnknown** o) override
        {
            if (!owner->dialogHost_) return gmpi::ReturnCode::NoSupport;
            const auto pr = gmpi::drawing::offsetRect(*r, offset);
            return owner->dialogHost_->createPopupMenu(&pr, o);
        }
        gmpi::ReturnCode createKeyListener(const gmpi::drawing::Rect* r, gmpi::api::IUnknown** o) override
        {
            if (!owner->dialogHost_) return gmpi::ReturnCode::NoSupport;
            const auto pr = gmpi::drawing::offsetRect(*r, offset);
            return owner->dialogHost_->createKeyListener(&pr, o);
        }
        gmpi::ReturnCode createFileDialog(int32_t t, gmpi::api::IUnknown** o) override
        {
            return owner->dialogHost_ ? owner->dialogHost_->createFileDialog(t, o) : gmpi::ReturnCode::NoSupport;
        }
        gmpi::ReturnCode createStockDialog(int32_t t, const char* a, const char* b, gmpi::api::IUnknown** o) override
        {
            return owner->dialogHost_ ? owner->dialogHost_->createStockDialog(t, a, b, o) : gmpi::ReturnCode::NoSupport;
        }
        gmpi::ReturnCode createColorDialog(gmpi::drawing::Color initialColor, gmpi::api::IUnknown** o) override
        {
            return owner->dialogHost_ ? owner->dialogHost_->createColorDialog(initialColor, o) : gmpi::ReturnCode::NoSupport;
        }

        gmpi::ReturnCode queryInterface(const gmpi::api::Guid* iid, void** returnInterface) override
        {
            *returnInterface = {};
            GMPI_QUERYINTERFACE(gmpi::api::IDrawingHost);
            GMPI_QUERYINTERFACE(gmpi::api::IInputHost);
            GMPI_QUERYINTERFACE(gmpi::api::IDialogHost);

            // The plugin's editor asks its host for IEditorHost during setHost;
            // that lives on the controller, which the window installed as the
            // frame's fallback. Forward anything we do not implement.
            if (owner->drawingHost_)
                return owner->drawingHost_->queryInterface(iid, returnInterface);

            return gmpi::ReturnCode::NoSupport;
        }
        GMPI_REFCOUNT_NO_DELETE;
    };

    struct Child
    {
        // Stable address required: the child holds a pointer to its host for
        // life, so the container below is a deque-like list of unique_ptrs
        // rather than a vector of values that reallocation would move.
        ChildHost host;
        gmpi::drawing::Rect pos{};
        gmpi::shared_ptr<gmpi::api::IDrawingClient> graphic;
        gmpi::shared_ptr<gmpi::api::IInputClient> input;
        gmpi::shared_ptr<gmpi::api::IGraphicsRedrawClient> redraw;
    };

    ~AppLayout();

    void setMenuBar(gmpi::api::IUnknown* client);
    void setMenuBarHeight(float h) { menuBarHeight_ = h; }

    // Returns the page index, for showPage().
    int addPage(gmpi::api::IUnknown* client);

    void showPage(int index);
    int  currentPage() const { return activePage_; }

    // --- IGraphicsRedrawClient ---
    // Forwarded to the visible page only: a hidden page's DSP->GUI queue can
    // wait, and a plugin editor that is not on screen should not be doing work.
    void preGraphicsRedraw() override;

    // --- IDrawingClient ---
    gmpi::ReturnCode setHost(gmpi::api::IUnknown* host) override;
    gmpi::ReturnCode measure(const gmpi::drawing::Size* availableSize, gmpi::drawing::Size* returnDesiredSize) override;
    gmpi::ReturnCode arrange(const gmpi::drawing::Rect* finalRect) override;
    gmpi::ReturnCode render(gmpi::drawing::api::IDeviceContext* dc) override;
    gmpi::ReturnCode getClipArea(gmpi::drawing::Rect* returnRect) override;

    // --- IInputClient ---
    gmpi::ReturnCode setHover(bool over) override;
    gmpi::ReturnCode hitTest(gmpi::drawing::Point p, int32_t flags) override;
    gmpi::ReturnCode onPointerDown(gmpi::drawing::Point p, int32_t flags) override;
    gmpi::ReturnCode onPointerMove(gmpi::drawing::Point p, int32_t flags) override;
    gmpi::ReturnCode onPointerUp(gmpi::drawing::Point p, int32_t flags) override;
    gmpi::ReturnCode onMouseWheel(gmpi::drawing::Point p, int32_t flags, int32_t delta) override;
    gmpi::ReturnCode populateContextMenu(gmpi::drawing::Point p, gmpi::api::IUnknown* sink) override;
    gmpi::ReturnCode onKeyPress(wchar_t c) override;
    gmpi::ReturnCode getToolTip(gmpi::drawing::Point p, gmpi::api::IString* s) override;

    gmpi::ReturnCode queryInterface(const gmpi::api::Guid* iid, void** returnInterface) override
    {
        *returnInterface = {};
        GMPI_QUERYINTERFACE(gmpi::api::IDrawingClient);
        GMPI_QUERYINTERFACE(gmpi::api::IInputClient);
        GMPI_QUERYINTERFACE(gmpi::api::IGraphicsRedrawClient);
        return gmpi::ReturnCode::NoSupport;
    }
    GMPI_REFCOUNT;

private:
    void attach(Child& child, gmpi::api::IUnknown* client);
    void arrangeChild(Child& child);
    void renderChild(gmpi::drawing::Graphics& g, gmpi::drawing::api::IDeviceContext* dc,
                     Child& child, const gmpi::drawing::Matrix3x2& base);

    Child* activeContent();
    Child* childAt(gmpi::drawing::Point p);
    static gmpi::drawing::Point toLocal(const Child* c, gmpi::drawing::Point p)
    {
        return { p.x - c->pos.left, p.y - c->pos.top };
    }

    gmpi::shared_ptr<gmpi::api::IDrawingHost> drawingHost_;
    gmpi::shared_ptr<gmpi::api::IInputHost> inputHost_;
    gmpi::shared_ptr<gmpi::api::IDialogHost> dialogHost_;

    Child menuBar_;
    std::vector<std::unique_ptr<Child>> pages_;
    int activePage_ = 0;

    float menuBarHeight_ = 26.0f;
    gmpi::drawing::Rect bounds_{};

    // Routes move/up to whichever child took the pointer-down, so a drag that
    // leaves a child's rectangle still reaches it.
    Child* capturedChild_{};
};

} // namespace standalone
} // namespace gmpi
