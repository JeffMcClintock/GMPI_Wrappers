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

    auto* back   = brush({ 0.18f, 0.19f, 0.21f, 1.f });
    auto* ink    = brush({ 0.88f, 0.89f, 0.92f, 1.f });
    auto* hilite = brush({ 0.28f, 0.42f, 0.66f, 1.f });

    if (back)
        dc->fillRectangle(&bounds_, back);

    for (size_t i = 0; i < menus_.size(); ++i)
    {
        const auto& m = menus_[i];

        // The open menu stays highlighted while its drop-down is up, so it is
        // obvious which one you are looking at.
        if (hilite && (int(i) == open_ || int(i) == hovered_))
            dc->fillRectangle(&m.rect, hilite);

        if (font_ && ink)
        {
            const gmpi::drawing::Rect textRect{ m.rect.left + kPadding, m.rect.top + kTextInset,
                                                m.rect.right, m.rect.bottom };
            dc->drawTextU(m.title.c_str(), uint32_t(m.title.size()), font_, &textRect, ink, 0);
        }
    }

    if (back)   back->release();
    if (ink)    ink->release();
    if (hilite) hilite->release();

    return gmpi::ReturnCode::Ok;
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
