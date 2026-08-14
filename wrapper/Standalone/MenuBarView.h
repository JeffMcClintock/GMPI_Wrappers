#pragma once

// The application menu bar, drawn.
//
// Copied with light edits from SynthEdit's SynthEditWayland/WaylandMenuBar -
// it depends on nothing but gmpi_ui, so it ports to Windows and macOS as-is
// even though those platforms have a native menu bar available. Whether to use
// the native one there is a decision for when those shells are written; this
// at least means the Linux app is not the only one that works.
//
// Wayland has no menu bar and no widget toolkit, so this is a gmpi_ui drawing
// client like everything else in the window: it paints its own titles, tracks
// hover, and opens the drop-downs through IDialogHost::createPopupMenu - the
// same popup path a plugin's right-click menu uses.
//
// It sits in AppLayout's strip, which means the ChildHost there maps the
// popup's anchor rect into window coordinates. Without that the drop-downs
// would open at the top-left corner regardless of which title was clicked.

#include <functional>
#include <string>
#include <vector>

#include "GmpiUiDrawing.h"
#include "GmpiApiCommon.h"
#include "GmpiSdkCommon.h"
#include "helpers/NativeUi.h"

namespace gmpi
{
namespace standalone
{

class MenuBarView :
      public gmpi::api::IDrawingClient
    , public gmpi::api::IInputClient
{
public:
    // What a menu item does when chosen. A plain callback rather than a command
    // enum, so the app can bind whatever it likes without this class knowing
    // anything about plugins, audio devices or settings.
    struct Item
    {
        std::string label;
        std::function<void()> action;   // empty => separator
        std::function<bool()> enabled;  // empty => always enabled
        // Draws a checkmark when it returns true. Like `enabled`, evaluated as
        // the menu opens, so an item mirroring a setting stays in step with no
        // notification.
        std::function<bool()> checked;  // empty => never ticked
    };

    struct Menu
    {
        std::string title;
        std::vector<Item> items;
        gmpi::drawing::Rect rect{};     // filled in by arrange()
    };

    static constexpr float kHeight = 26.0f;

    void setMenus(std::vector<Menu> menus) { menus_ = std::move(menus); }
    void setFont(gmpi::drawing::api::ITextFormat* f) { font_ = f; }

    // --- IDrawingClient ---
    gmpi::ReturnCode setHost(gmpi::api::IUnknown* host) override;
    gmpi::ReturnCode measure(const gmpi::drawing::Size* available,
                             gmpi::drawing::Size* returnDesired) override
    {
        *returnDesired = { available ? available->width : 0.f, kHeight };
        return gmpi::ReturnCode::Ok;
    }
    gmpi::ReturnCode arrange(const gmpi::drawing::Rect* finalRect) override;
    gmpi::ReturnCode render(gmpi::drawing::api::IDeviceContext* dc) override;
    gmpi::ReturnCode getClipArea(gmpi::drawing::Rect* returnRect) override
    {
        *returnRect = bounds_;
        return gmpi::ReturnCode::Ok;
    }

    // --- IInputClient ---
    gmpi::ReturnCode setHover(bool over) override;
    gmpi::ReturnCode hitTest(gmpi::drawing::Point, int32_t) override { return gmpi::ReturnCode::Ok; }
    gmpi::ReturnCode onPointerDown(gmpi::drawing::Point p, int32_t flags) override;
    gmpi::ReturnCode onPointerMove(gmpi::drawing::Point p, int32_t flags) override;
    gmpi::ReturnCode onPointerUp(gmpi::drawing::Point, int32_t) override { return gmpi::ReturnCode::Ok; }
    gmpi::ReturnCode onMouseWheel(gmpi::drawing::Point, int32_t, int32_t) override
    { return gmpi::ReturnCode::Unhandled; }
    gmpi::ReturnCode populateContextMenu(gmpi::drawing::Point, gmpi::api::IUnknown*) override
    { return gmpi::ReturnCode::Unhandled; }   // the menu bar has no context menu of its own
    gmpi::ReturnCode onKeyPress(wchar_t) override { return gmpi::ReturnCode::Unhandled; }
    gmpi::ReturnCode getToolTip(gmpi::drawing::Point, gmpi::api::IString*) override
    { return gmpi::ReturnCode::Unhandled; }

    gmpi::ReturnCode queryInterface(const gmpi::api::Guid* iid, void** returnInterface) override
    {
        *returnInterface = {};
        GMPI_QUERYINTERFACE(gmpi::api::IDrawingClient);
        GMPI_QUERYINTERFACE(gmpi::api::IInputClient);
        return gmpi::ReturnCode::NoSupport;
    }
    GMPI_REFCOUNT;

private:
    void openMenu(int index);
    int  menuAt(gmpi::drawing::Point p) const;
    void invalidate();

    std::vector<Menu> menus_;
    gmpi::drawing::api::ITextFormat* font_{};
    gmpi::drawing::Rect bounds_{};

    gmpi::api::IUnknown* host_{};
    int hovered_ = -1;
    int open_    = -1;   // which drop-down is showing, or -1
};

} // namespace standalone
} // namespace gmpi
