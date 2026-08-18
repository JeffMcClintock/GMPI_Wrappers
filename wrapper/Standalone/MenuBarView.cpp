#include "MenuBarView.h"

#include <algorithm>

namespace gmpi
{
namespace standalone
{

namespace
{
constexpr float kPadding   = 14.0f;   // either side of a title
constexpr float kTextInset = 5.0f;    // pushes glyphs down to sit optically centred
}

gmpi::ReturnCode MenuBarView::setHost(gmpi::api::IUnknown* host)
{
    host_ = host;
    return gmpi::ReturnCode::Ok;
}

void MenuBarView::invalidate()
{
    gmpi::api::IDrawingHost* h{};
    if (host_ && host_->queryInterface(&gmpi::api::IDrawingHost::guid,
                                       reinterpret_cast<void**>(&h)) == gmpi::ReturnCode::Ok)
    {
        h->invalidateRect(nullptr);
        h->release();
    }
}

gmpi::ReturnCode MenuBarView::arrange(const gmpi::drawing::Rect* finalRect)
{
    bounds_ = *finalRect;

    // Titles are laid out left to right, each as wide as its text plus padding.
    // Measured rather than guessed: "Options" and "File" are not the same
    // width, and a fixed stride would put the hit rectangles in the wrong
    // places - the drop-down would open under the wrong heading.
    float x = bounds_.left + 4.0f;
    for (auto& m : menus_)
    {
        float width = 60.0f;
        if (font_)
        {
            gmpi::drawing::Size sz{};
            if (font_->getTextExtentU(m.title.c_str(), int32_t(m.title.size()),
                                      100000.f, &sz) == gmpi::ReturnCode::Ok)
                width = sz.width + 2 * kPadding;
        }

        m.rect = { x, bounds_.top, x + width, bounds_.top + kHeight };
        x += width;
    }

    return gmpi::ReturnCode::Ok;
}

int MenuBarView::menuAt(gmpi::drawing::Point p) const
{
    for (size_t i = 0; i < menus_.size(); ++i)
    {
        const auto& r = menus_[i].rect;
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom)
            return int(i);
    }
    return -1;
}

gmpi::ReturnCode MenuBarView::render(gmpi::drawing::api::IDeviceContext* dc)
{
    auto brush = [&](gmpi::drawing::Color c)
    {
        gmpi::drawing::api::ISolidColorBrush* b{};
        dc->createSolidColorBrush(&c, nullptr, &b);
        return b;
    };

    // Read every frame rather than cached, which is what makes a theme change
    // free: the brushes are built here anyway, so the only thing a change needs
    // is a repaint (preGraphicsRedraw, below).
    //
    // panelBackground is the CHROME colour - what gmpi_ui paints a panel with,
    // as against controlBackground, which is a control's body. A menu bar is
    // chrome, so the strip takes the former.
    //
    // The highlight is `accent`, WITH its own ink. controlBackground was tried
    // first and is not a highlight: it sits about a dozen sRGB levels from
    // panelBackground in both modes, which is right for a combo box on a panel
    // and reads as a smudge on a menu title (1.2:1, against 2.7:1 for accent in
    // the dark theme and 4.9:1 in the light one). accent/accentText were added
    // to ColorTheme for this rather than picked here, because "the colour that
    // marks the current thing" is a question every widget set has and hard-
    // coding one is what this change exists to stop.
    const auto& theme = gmpi::ui::currentTheme();

    auto* back      = brush(theme.panelBackground);
    auto* ink       = brush(theme.controlText);
    auto* hilite    = brush(theme.accent);
    auto* hiliteInk = brush(theme.accentText);

    if (back)
        dc->fillRectangle(&bounds_, back);

    for (size_t i = 0; i < menus_.size(); ++i)
    {
        const auto& m = menus_[i];

        // The open menu stays highlighted while its drop-down is up, so it is
        // obvious which one you are looking at.
        const bool lit = (int(i) == open_ || int(i) == hovered_);

        if (hilite && lit)
            dc->fillRectangle(&m.rect, hilite);

        // The title switches ink with its background. accent is a mid-tone in
        // both modes, so the light theme's near-black controlText on it would be
        // the legibility problem the highlight is meant to solve.
        auto* titleInk = (lit && hiliteInk) ? hiliteInk : ink;

        if (font_ && titleInk)
        {
            const gmpi::drawing::Rect textRect{ m.rect.left + kPadding, m.rect.top + kTextInset,
                                                m.rect.right, m.rect.bottom };
            dc->drawTextU(m.title.c_str(), uint32_t(m.title.size()), font_, &textRect, titleInk, 0);
        }
    }

    if (back)      back->release();
    if (ink)       ink->release();
    if (hilite)    hilite->release();
    if (hiliteInk) hiliteInk->release();

    return gmpi::ReturnCode::Ok;
}

void MenuBarView::preGraphicsRedraw()
{
    // setThemeMode invalidates nothing - it bumps a counter and leaves the
    // repainting to whoever notices - so this is the noticing. render() picks
    // the new palette up by itself; the only thing missing is a render, and a
    // window nobody is touching does not draw one. Measured: 52 idle ticks
    // produced no repaint at all, and the tick after the mode changed produced
    // one.
    //
    // invalidate() marks the WHOLE window rather than the strip, because
    // ChildHost passes a null rect straight through to the frame. THAT IS LOAD
    // BEARING FOR THE PAGE BELOW, not just convenient: SettingsPane watches the
    // theme counter only from inside its own render(), so with nothing asking
    // for a render it never looks. Narrow this to bounds_ and the strip alone
    // will follow a theme change while the page under it stays in the old
    // palette (SettingsPane::render says the same from the other end).
    if (gmpi::ui::consumeThemeChanged(lastSeenThemeVersion_))
        invalidate();
}

gmpi::ReturnCode MenuBarView::setHover(bool over)
{
    if (!over && hovered_ != -1)
    {
        hovered_ = -1;
        invalidate();
    }
    return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode MenuBarView::onPointerMove(gmpi::drawing::Point p, int32_t)
{
    const int hit = menuAt(p);
    if (hit != hovered_)
    {
        hovered_ = hit;
        invalidate();
    }
    return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode MenuBarView::onPointerDown(gmpi::drawing::Point p, int32_t)
{
    const int hit = menuAt(p);
    if (hit < 0)
        return gmpi::ReturnCode::Unhandled;

    openMenu(hit);
    return gmpi::ReturnCode::Ok;
}

void MenuBarView::openMenu(int index)
{
    if (index < 0 || index >= int(menus_.size()) || !host_)
        return;

    gmpi::api::IDialogHost* dialogHost{};
    if (host_->queryInterface(&gmpi::api::IDialogHost::guid,
                              reinterpret_cast<void**>(&dialogHost)) != gmpi::ReturnCode::Ok)
        return;

    gmpi::shared_ptr<gmpi::api::IDialogHost> hostRef;
    hostRef.attach(dialogHost);   // queryInterface already addRef'd

    auto& menu = menus_[size_t(index)];

    // Anchor on the TITLE, not the pointer: a menu bar drop-down lines up with
    // its heading. AppLayout maps this into window coordinates.
    gmpi::api::IUnknown* raw{};
    if (hostRef->createPopupMenu(&menu.rect, &raw) != gmpi::ReturnCode::Ok || !raw)
        return;

    gmpi::shared_ptr<gmpi::api::IUnknown> owner;
    owner.attach(raw);

    auto popup = owner.as<gmpi::api::IPopupMenu>();
    if (!popup)
        return;

    using F = gmpi::api::PopupMenuFlags;

    // Heap-allocated, NOT a member.
    //
    // PopupMenuCallback carries GMPI_REFCOUNT, so it deletes itself when the
    // count reaches zero - and addItem takes a reference. As a member it was
    // destroyed in place the moment the menu let go, which corrupted the heap
    // and later crashed on a call through the wrecked vtable. One object serves
    // the whole drop-down; the menu owns it once we drop our reference below.
    auto* callback = new gmpi::sdk::PopupMenuCallback(
        [this, index](int32_t selectedId)
        {
            open_ = -1;
            invalidate();

            auto& m = menus_[size_t(index)];
            const size_t itemIndex = size_t(selectedId - 1);
            if (selectedId <= 0 || itemIndex >= m.items.size())
                return;

            auto& item = m.items[itemIndex];
            if (item.action && (!item.enabled || item.enabled()))
                item.action();
        },
        [this]
        {
            open_ = -1;
            invalidate();
        });

    for (size_t i = 0; i < menu.items.size(); ++i)
    {
        const auto& item = menu.items[i];

        if (!item.action)
        {
            popup->addItem("", 0, int32_t(F::Separator), nullptr);
            continue;
        }

        const bool enabled = !item.enabled || item.enabled();
        const bool ticked  = item.checked && item.checked();
        popup->addItem(item.label.c_str(), int32_t(i + 1),
                       (enabled ? 0 : int32_t(F::Grayed)) | (ticked ? int32_t(F::Ticked) : 0),
                       callback);
    }

    // Every item took a reference; drop ours so the menu owns it outright and
    // it dies with the menu rather than leaking or outliving it.
    callback->release();

    open_ = index;
    invalidate();
    popup->showAsync();
}

} // namespace standalone
} // namespace gmpi
