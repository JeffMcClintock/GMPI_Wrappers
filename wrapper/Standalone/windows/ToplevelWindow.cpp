#include "ToplevelWindow.h"

#include <algorithm>

#include "helpers/unicode_conversion.h"

namespace gmpi
{
namespace standalone
{

namespace
{

// A resizable window with the usual caption and buttons. Named once because
// three places need it to agree: the CreateWindowEx call, and both
// AdjustWindowRectExForDpi calls that convert a client size into a window size.
// A mismatch there is a window a few pixels wrong on every DPI but 96.
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;

constexpr wchar_t kWindowClassName[] = L"GmpiStandaloneToplevel";

// Registered once per process. CS_DBLCLKS is deliberately absent: the frame
// synthesises double-clicks from timing and movement (see the header comment on
// DxDrawingFrameHwnd::lastLButtonDownTick) precisely so it does not depend on
// whichever window class a host happened to register, and turning the real
// message on here would give the editor two double-click paths to disagree.
bool registerWindowClass(HINSTANCE instance, WNDPROC windowProc)
{
    static bool registered = false;
    if (registered)
        return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = windowProc;
    wc.hInstance     = instance;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = ::LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kWindowClassName;

    // No hbrBackground. The frame's child window covers the whole client area
    // and paints every pixel of it, so a background brush would only ever be
    // visible as a flash of grey during a resize.
    wc.hbrBackground = nullptr;

    registered = ::RegisterClassExW(&wc) != 0;
    return registered;
}

HINSTANCE thisModule()
{
    HMODULE module{};
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&thisModule),
        &module);
    return module;
}

} // namespace

ToplevelWindow::~ToplevelWindow()
{
    close();
}

UINT ToplevelWindow::dpi() const
{
    // GetDpiForWindow is per-monitor and updates as the window moves, which is
    // the whole point; before the window exists there is nothing to ask, and
    // the primary monitor's DPI is the best available guess for where it will
    // open. Any error is corrected in create(), which re-reads the real DPI
    // once the window has landed.
    return hwnd_ ? ::GetDpiForWindow(hwnd_) : ::GetDpiForSystem();
}

bool ToplevelWindow::create(const std::string& title, int clientWidthDips, int clientHeightDips)
{
    const auto instance = thisModule();
    if (!registerWindowClass(instance, &ToplevelWindow::windowProc))
        return false;

    const auto wideTitle = gmpi::unicode::to_wide(title);

    // Created with a guessed size, then corrected below. There is no way to ask
    // "what DPI will this window open at" without a window, and a per-monitor
    // aware app on a mixed-DPI desktop opens on whichever monitor the shell
    // decides - which is not necessarily the primary one this guess assumes.
    const UINT guessedDpi = ::GetDpiForSystem();

    RECT r{ 0, 0,
            ::MulDiv(clientWidthDips,  static_cast<int>(guessedDpi), 96),
            ::MulDiv(clientHeightDips, static_cast<int>(guessedDpi), 96) };
    ::AdjustWindowRectExForDpi(&r, kWindowStyle, FALSE, 0, guessedDpi);

    hwnd_ = ::CreateWindowExW(
        0,
        kWindowClassName,
        wideTitle.c_str(),
        kWindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, instance,
        this);            // -> WM_NCCREATE, which installs the back-pointer

    if (!hwnd_)
        return false;

    // The correction. If the window opened on a monitor whose DPI differs from
    // the guess, the client area is now the wrong size for the editor - so ask
    // the window where it actually is and redo the arithmetic. Done before the
    // frame is created, so the swap chain is only ever sized once.
    if (const UINT actualDpi = ::GetDpiForWindow(hwnd_); actualDpi != guessedDpi)
    {
        RECT corrected{ 0, 0,
                        ::MulDiv(clientWidthDips,  static_cast<int>(actualDpi), 96),
                        ::MulDiv(clientHeightDips, static_cast<int>(actualDpi), 96) };
        ::AdjustWindowRectExForDpi(&corrected, kWindowStyle, FALSE, 0, actualDpi);

        ::SetWindowPos(hwnd_, nullptr, 0, 0,
                       corrected.right - corrected.left,
                       corrected.bottom - corrected.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // The frame sizes itself to the parent's client rect and starts its own
    // redraw timer. From here on the window's contents are its business.
    frame_.open(hwnd_);
    if (!frame_.getWindowHandle())
    {
        close();
        return false;
    }

    return true;
}

void ToplevelWindow::setMinimumClientSize(int widthDips, int heightDips)
{
    minClientWidthDips_  = widthDips;
    minClientHeightDips_ = heightDips;
}

void ToplevelWindow::close()
{
    // Frame first. DestroyWindow on the parent destroys the child too, and the
    // frame must be the one to do that: it has to clear its window-proc
    // back-pointer and stop its timer before the HWND goes, or a message still
    // in flight dispatches through a half-destroyed frame.
    frame_.close();

    if (hwnd_)
    {
        // The back-pointer is deliberately LEFT IN PLACE across this call, and
        // hwnd_ is not cleared here either. DestroyWindow sends WM_DESTROY
        // synchronously, and that handler is what posts WM_QUIT - detaching
        // first would route it to DefWindowProc instead, the event loop would
        // never be told to stop, and closing the window would hang the app with
        // no window and a live message pump. windowProc clears both on
        // WM_NCDESTROY, which is the last message this HWND can ever deliver.
        ::DestroyWindow(hwnd_);
    }
}

void ToplevelWindow::resizeFrameToClient()
{
    if (!hwnd_)
        return;

    RECT client{};
    ::GetClientRect(hwnd_, &client);

    frame_.reSize(0, 0, client.right, client.bottom);
}

int ToplevelWindow::runEventLoop()
{
    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK ToplevelWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // WM_NCCREATE is the first message a window receives, and the only place the
    // instance pointer is available - it travels in CREATESTRUCT rather than in
    // any of the messages that follow.
    if (message == WM_NCCREATE)
    {
        auto* const create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* const self = reinterpret_cast<ToplevelWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self)
        return ::DefWindowProcW(hwnd, message, wParam, lParam);

    // WM_NCDESTROY is the LAST message an HWND can deliver, so it is the only
    // safe place to detach: anything earlier (WM_CLOSE, WM_DESTROY) still has
    // work for this object to do. Done here rather than in onMessage because
    // this is where the handle is - onMessage's copy is cleared by WM_DESTROY.
    if (message == WM_NCDESTROY)
    {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = {};
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // Messages arriving before CreateWindowEx has returned reach here with the
    // back-pointer set but hwnd_ still empty. Fill it in rather than dropping
    // them: WM_CREATE and the first WM_SIZE both land in that window.
    if (!self->hwnd_)
        self->hwnd_ = hwnd;

    return self->onMessage(hwnd, message, wParam, lParam);
}

LRESULT ToplevelWindow::onMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_SIZE:
        // SIZE_MINIMIZED reports a client area of 0x0, which is not a size any
        // swap chain can be made to satisfy - and restoring sends a second
        // WM_SIZE with the real one anyway.
        if (wParam != SIZE_MINIMIZED)
            resizeFrameToClient();
        return 0;

    case WM_GETMINMAXINFO:
    {
        if (minClientWidthDips_ <= 0 && minClientHeightDips_ <= 0)
            break;

        const auto d = static_cast<int>(dpi());

        RECT r{ 0, 0,
                ::MulDiv(minClientWidthDips_,  d, 96),
                ::MulDiv(minClientHeightDips_, d, 96) };
        ::AdjustWindowRectExForDpi(&r, kWindowStyle, FALSE, 0, static_cast<UINT>(d));

        auto* const mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_DPICHANGED:
    {
        // Windows has already worked out the rectangle that keeps the window
        // the same physical size on the new monitor; taking it is what the
        // per-monitor-v2 contract asks of us. The WM_SIZE that follows resizes
        // the frame, and the frame re-reads its own DPI when it does.
        const auto* const suggested = reinterpret_cast<const RECT*>(lParam);
        ::SetWindowPos(hwnd, nullptr,
                       suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SETFOCUS:
        // Keyboard input belongs to the frame: it is the window that turns
        // WM_CHAR into IInputClient::onKeyPress. A top-level window that kept
        // focus for itself would leave the plugin's text entry mute.
        if (const HWND child = frame_.getWindowHandle())
            ::SetFocus(child);
        return 0;

    case WM_ERASEBKGND:
        // The frame covers every pixel. Letting the default handler paint first
        // buys a grey flash on each resize and nothing else.
        return 1;

    case WM_CLOSE:
        close();
        return 0;

    case WM_DESTROY:
        // The app's only window, so its destruction ends the process. hwnd_ is
        // cleared here (not in close()) so that close(), the destructor and a
        // user-driven close all converge on the same path.
        hwnd_ = {};
        ::PostQuitMessage(0);
        return 0;
    }

    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace standalone
} // namespace gmpi
