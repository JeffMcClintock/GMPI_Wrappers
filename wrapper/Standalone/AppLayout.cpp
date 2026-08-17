#include "AppLayout.h"

namespace gmpi
{
namespace standalone
{

AppLayout::~AppLayout()
{
    // Sever children before their ChildHosts are destroyed, so a child that
    // outlives this layout drops its host pointer rather than dangling.
    if (menuBar_.graphic)
        menuBar_.graphic->setHost(nullptr);

    for (auto& page : pages_)
    {
        if (page->graphic)
            page->graphic->setHost(nullptr);
    }
}

void AppLayout::attach(Child& child, gmpi::api::IUnknown* client)
{
    child.host.owner = this;

    // A plugin with no GUI hands us nothing; the page then draws nothing and
    // the window is still usable, which is the point of having a menu bar.
    if (!client)
        return;

    gmpi::shared_ptr<gmpi::api::IUnknown> u;
    u = client;

    child.graphic = u.as<gmpi::api::IDrawingClient>();
    child.input   = u.as<gmpi::api::IInputClient>();
    child.redraw  = u.as<gmpi::api::IGraphicsRedrawClient>();

    if (child.graphic)
        child.graphic->setHost(static_cast<gmpi::api::IDrawingHost*>(&child.host));
}

void AppLayout::setMenuBar(gmpi::api::IUnknown* client)
{
    attach(menuBar_, client);
}

int AppLayout::addPage(gmpi::api::IUnknown* client)
{
    pages_.push_back(std::make_unique<Child>());
    attach(*pages_.back(), client);
    return static_cast<int>(pages_.size()) - 1;
}

void AppLayout::showPage(int index)
{
    if (index < 0 || index >= static_cast<int>(pages_.size()) || index == activePage_)
        return;

    activePage_ = index;

    // A page that was never on screen has never been arranged, and one that
    // was arranged for a different window size is stale either way.
    if (auto* page = activeContent())
        arrangeChild(*page);

    capturedChild_ = nullptr;   // the page that held the pointer is gone

    if (drawingHost_)
        drawingHost_->invalidateRect(nullptr);
}

AppLayout::Child* AppLayout::activeContent()
{
    if (activePage_ < 0 || activePage_ >= static_cast<int>(pages_.size()))
        return nullptr;

    return pages_[static_cast<size_t>(activePage_)].get();
}

void AppLayout::preGraphicsRedraw()
{
    if (menuBar_.redraw)
        menuBar_.redraw->preGraphicsRedraw();

    if (auto* page = activeContent(); page && page->redraw)
        page->redraw->preGraphicsRedraw();
}

gmpi::ReturnCode AppLayout::setHost(gmpi::api::IUnknown* host)
{
    gmpi::shared_ptr<gmpi::api::IUnknown> u;
    u = host;

    drawingHost_ = u.as<gmpi::api::IDrawingHost>();
    inputHost_   = u.as<gmpi::api::IInputHost>();
    dialogHost_  = u.as<gmpi::api::IDialogHost>();

    // Re-offer every child its host, now that ours resolves.
    //
    // ChildHost answers IDrawingHost/IInputHost/IDialogHost itself and forwards
    // everything else to drawingHost_ - notably IEditorHost, which is where a
    // plugin's parameter pins get the host they WRITE through. Until the lines
    // above, drawingHost_ was null and that forward returned NoSupport, so a
    // child attached earlier cached a null host in every pin (PinBase::host,
    // bound once in PluginEditorBase::setHost) and the first knob drag
    // dereferenced it.
    //
    // That is the ordinary order rather than a corner case: the app builds its
    // pages first (addPage -> attach -> the child's setHost) and only then
    // hands the finished layout to the frame, which is what calls us. Reading
    // values worked the whole time - that direction is host->pin and needs no
    // host pointer - so the gap only showed up as a segfault the moment
    // anything dragged a control.
    //
    // Guarded on drawingHost_ so that setHost(nullptr) at teardown does not
    // re-bind children to a ChildHost whose forwarding is already dead; the
    // destructor severs them explicitly. A client's setHost is idempotent (it
    // re-queries and reassigns), so calling it a second time is safe.
    if (drawingHost_)
    {
        if (menuBar_.graphic)
            menuBar_.graphic->setHost(static_cast<gmpi::api::IDrawingHost*>(&menuBar_.host));

        for (auto& page : pages_)
        {
            if (page->graphic)
                page->graphic->setHost(static_cast<gmpi::api::IDrawingHost*>(&page->host));
        }
    }

    return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode AppLayout::measure(const gmpi::drawing::Size* availableSize, gmpi::drawing::Size* returnDesiredSize)
{
    // The window sizes itself from the plugin's editor, which the app asks
    // directly before creating the window; this layout simply fills whatever
    // it is given.
    if (returnDesiredSize && availableSize)
        *returnDesiredSize = *availableSize;

    return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode AppLayout::arrange(const gmpi::drawing::Rect* finalRect)
{
    bounds_ = *finalRect;
    const auto& r = bounds_;

    // Never let the strip eat the whole window: a content area of zero (or
    // negative, which inverts the rect) is not a layout a compositor should be
    // able to force on us by making the window one pixel tall.
    const float strip = (std::max)(0.0f, (std::min)(menuBarHeight_, r.bottom - r.top));

    menuBar_.pos = { r.left, r.top, r.right, r.top + strip };
    arrangeChild(menuBar_);

    const gmpi::drawing::Rect contentPos{ r.left, r.top + strip, r.right, r.bottom };
    for (auto& page : pages_)
        page->pos = contentPos;

    // Only the visible page. Arranging a hidden plugin editor costs a full
    // layout pass for something nobody is looking at; showPage does it when
    // the page becomes visible.
    if (auto* page = activeContent())
        arrangeChild(*page);

    return gmpi::ReturnCode::Ok;
}

void AppLayout::arrangeChild(Child& child)
{
    if (!child.graphic)
        return;

    child.host.offset = { child.pos.left, child.pos.top };

    // The child works in (0,0)-origin local coordinates; the layout positions it.
    const gmpi::drawing::Rect local
    {
        0.0f, 0.0f,
        child.pos.right - child.pos.left,
        child.pos.bottom - child.pos.top
    };

    child.graphic->arrange(&local);
}

gmpi::ReturnCode AppLayout::render(gmpi::drawing::api::IDeviceContext* dc)
{
    gmpi::drawing::Graphics g(dc);
    const auto base = g.getTransform();

    if (auto* page = activeContent())
        renderChild(g, dc, *page, base);

    renderChild(g, dc, menuBar_, base);

    g.setTransform(base);
    return gmpi::ReturnCode::Ok;
}

void AppLayout::renderChild(
    gmpi::drawing::Graphics& g,
    gmpi::drawing::api::IDeviceContext* dc,
    Child& child,
    const gmpi::drawing::Matrix3x2& base)
{
    if (!child.graphic)
        return;

    g.setTransform(gmpi::drawing::makeTranslation(child.pos.left, child.pos.top) * base);

    const gmpi::drawing::Rect local
    {
        0.0f, 0.0f,
        child.pos.right - child.pos.left,
        child.pos.bottom - child.pos.top
    };

    g.pushAxisAlignedClip(local);
    child.graphic->render(dc);
    g.popAxisAlignedClip();
}

gmpi::ReturnCode AppLayout::getClipArea(gmpi::drawing::Rect* returnRect)
{
    if (returnRect)
        *returnRect = bounds_;

    return gmpi::ReturnCode::Ok;
}

AppLayout::Child* AppLayout::childAt(gmpi::drawing::Point p)
{
    if (menuBar_.input && pointInRect(p, menuBar_.pos))
        return &menuBar_;

    if (auto* page = activeContent(); page && page->input && pointInRect(p, page->pos))
        return page;

    return nullptr;
}

gmpi::ReturnCode AppLayout::setHover(bool over)
{
    // Leaving the window means nothing is hovered; tell whichever child last
    // thought it was, or its highlight sticks until the pointer returns.
    if (!over)
    {
        if (menuBar_.input)
            menuBar_.input->setHover(false);

        if (auto* page = activeContent(); page && page->input)
            page->input->setHover(false);
    }

    return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode AppLayout::hitTest(gmpi::drawing::Point p, int32_t flags)
{
    auto* child = childAt(p);
    return (child && child->input->hitTest(toLocal(child, p), flags) == gmpi::ReturnCode::Ok)
         ? gmpi::ReturnCode::Ok
         : gmpi::ReturnCode::Unhandled;
}

gmpi::ReturnCode AppLayout::onPointerDown(gmpi::drawing::Point p, int32_t flags)
{
    auto* child = childAt(p);
    capturedChild_ = child;

    return child ? child->input->onPointerDown(toLocal(child, p), flags) : gmpi::ReturnCode::Unhandled;
}

gmpi::ReturnCode AppLayout::onPointerMove(gmpi::drawing::Point p, int32_t flags)
{
    // While a child holds capture every move belongs to it, wherever the
    // pointer is - that is the whole point of capture, and hit-testing the
    // position instead would drop a knob drag the moment it left the control.
    Child* child = capturedChild_;

    if (!child)
    {
        bool held = false;
        if (inputHost_)
            inputHost_->getCapture(held);

        child = held ? activeContent() : childAt(p);
    }

    // Hovering moved from one child to the other: the one being left has to be
    // told, or its highlight stays lit.
    if (!capturedChild_)
    {
        Child* other = (child == &menuBar_) ? activeContent() : &menuBar_;
        if (other && other->input)
            other->input->setHover(false);
    }

    return (child && child->input) ? child->input->onPointerMove(toLocal(child, p), flags)
                                   : gmpi::ReturnCode::Unhandled;
}

gmpi::ReturnCode AppLayout::onPointerUp(gmpi::drawing::Point p, int32_t flags)
{
    Child* child = capturedChild_ ? capturedChild_ : childAt(p);

    const auto r = (child && child->input) ? child->input->onPointerUp(toLocal(child, p), flags)
                                           : gmpi::ReturnCode::Unhandled;

    // Track the child's LOGICAL capture rather than clearing on every raw
    // button-up: a gesture can survive one. Checked AFTER forwarding, because
    // the child releases capture from inside its own onPointerUp.
    bool held = false;
    if (inputHost_)
        inputHost_->getCapture(held);

    if (!held)
        capturedChild_ = nullptr;

    return r;
}

gmpi::ReturnCode AppLayout::onMouseWheel(gmpi::drawing::Point p, int32_t flags, int32_t delta)
{
    auto* child = childAt(p);
    return (child && child->input) ? child->input->onMouseWheel(toLocal(child, p), flags, delta)
                                   : gmpi::ReturnCode::Unhandled;
}

gmpi::ReturnCode AppLayout::populateContextMenu(gmpi::drawing::Point p, gmpi::api::IUnknown* sink)
{
    auto* child = childAt(p);
    return (child && child->input) ? child->input->populateContextMenu(toLocal(child, p), sink)
                                   : gmpi::ReturnCode::Unhandled;
}

gmpi::ReturnCode AppLayout::onKeyPress(wchar_t c)
{
    // Keys go to the content, never the menu bar: there is no keyboard focus
    // model here, and a menu bar with no open drop-down wants no keystrokes.
    auto* page = activeContent();
    return (page && page->input) ? page->input->onKeyPress(c) : gmpi::ReturnCode::Unhandled;
}

gmpi::ReturnCode AppLayout::getToolTip(gmpi::drawing::Point p, gmpi::api::IString* s)
{
    auto* child = childAt(p);
    return (child && child->input) ? child->input->getToolTip(toLocal(child, p), s)
                                   : gmpi::ReturnCode::Unhandled;
}

} // namespace standalone
} // namespace gmpi
