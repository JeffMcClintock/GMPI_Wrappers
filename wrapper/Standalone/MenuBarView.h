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
//
// Its colours come from gmpi::ui::currentTheme(), the same palette SettingsPane
// clears itself with. They used to be constants picked to look right in the
// dark, which cost exactly what drawing our own menu bar is meant to buy: with
// a light theme selected the window wore a permanently dark strip across the
// top of a light page, identically on all three platforms.
//
// NOTHING IN THE STANDALONE SELECTS A THEME YET. No shell asks the OS which
// mode it is in, and gmpi::ui's default is ThemeMode::Dark, so that is the
// palette this reads until a shell (or the hosted plugin) calls setThemeMode.
// Following the OS is shell work and belongs with the other per-platform
// questions in PlatformShell; what is fixed here is that the strip no longer
// has an opinion of its own about which mode it is in.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "GmpiUiDrawing.h"
#include "GmpiApiCommon.h"
#include "GmpiSdkCommon.h"
#include "experimental/theme.h"
#include "helpers/NativeUi.h"

namespace gmpi
{
namespace standalone
{

class MenuBarView :
      public gmpi::api::IDrawingClient
    , public gmpi::api::IInputClient
    , public gmpi::api::IGraphicsRedrawClient
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

    // --- IGraphicsRedrawClient ---
    // Only to notice a theme change. The bar has no animation and no DSP->GUI
    // queue to service; what it cannot do without a per-frame call is find out
    // that the palette moved while the window sat idle, because setThemeMode
    // invalidates nothing and on a still window nothing else repaints us.
    // AppLayout::preGraphicsRedraw forwards it to the strip and to the visible
    // page.
    void preGraphicsRedraw() override;

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
        GMPI_QUERYINTERFACE(gmpi::api::IGraphicsRedrawClient);
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

    // OUR OWN copy of the theme counter, which is why preGraphicsRedraw calls
    // the overload that takes one. The argument-less
    // gmpi::ui::consumeThemeChanged() keeps a single static shared by every
    // caller in the process, so of two views watching through it the first to
    // ask takes the change and the second is told nothing happened. Seeded from
    // the counter's current value so the first tick after construction reports
    // a change only if there really was one.
    uint32_t lastSeenThemeVersion_ = gmpi::ui::themeVersion();
};

} // namespace standalone
} // namespace gmpi
