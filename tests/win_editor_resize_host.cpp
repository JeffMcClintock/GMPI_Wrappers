// ---------------------------------------------------------------------------
// A minimal Win32 VST3 host that attaches a plugin editor and then asks it to
// resize to a size no device can satisfy. The regression test for the resize
// crash fixed in DrawingFrame::reSize and SEVSTGUIEditorWin::checkSizeConstraint.
//
// The crash was first seen by driving REAPER, and diagnosed from the minidump:
// the host called IPlugView::onSize({0, 0, 2178, 32672}), DrawingFrame::reSize
// checked its D2D device, called SetWindowPos -- which *sends* WM_SIZE, whose
// handler releases the device on a failed ResizeBuffers -- and then dereferenced
// the pointer it had checked before the call. Time-of-check/time-of-use across a
// re-entrant Win32 call, and it took the host process down.
//
// Driving a DAW could not be made to produce that rect a second time, so this
// applies the proven input directly instead. No DAW, no DAW state.
//
// Deliberately built from the pluginterfaces headers alone -- no SDK hosting
// classes, no funknown.cpp -- because ClassName_iid is a header-only constant.
// The whole test is one translation unit and user32.
//
// Usage:
//     win_editor_resize_host <plugin.vst3> [right] [bottom]
//
// Exit status:
//     0  survived the oversized onSize, and the editor was provably live
//     1  setup failed -- the message says which step
//     3  the editor was NOT provably live, so surviving proves nothing
//
// A regression shows up as the process dying with 0xC0000005, not as an exit
// code. Run it from a script that treats any nonzero status as failure.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"

using namespace Steinberg;

namespace
{

bool iidEqual(const TUID a, const TUID b) { return std::memcmp(a, b, 16) == 0; }

// ---------------------------------------------------------------------------
// The host side, minimally: IHostApplication so initialize() succeeds,
// IComponentHandler because a controller may ask for one, and IPlugFrame
// because a well-behaved view expects a frame before it is attached.
// ---------------------------------------------------------------------------
class Host : public Vst::IHostApplication, public Vst::IComponentHandler, public IPlugFrame
{
public:
	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
	{
		if (iidEqual(iid, FUnknown_iid) || iidEqual(iid, Vst::IHostApplication_iid))
			{ *obj = static_cast<Vst::IHostApplication*>(this); return kResultOk; }
		if (iidEqual(iid, Vst::IComponentHandler_iid))
			{ *obj = static_cast<Vst::IComponentHandler*>(this); return kResultOk; }
		if (iidEqual(iid, IPlugFrame_iid))
			{ *obj = static_cast<IPlugFrame*>(this); return kResultOk; }
		*obj = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }

	tresult PLUGIN_API getName(Vst::String128 name) override
	{
		static const char16_t n[] = u"win_editor_resize_host";
		std::memcpy(name, n, sizeof(n));
		return kResultOk;
	}
	tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override
	{
		*obj = nullptr;
		return kNotImplemented;
	}

	tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
	tresult PLUGIN_API performEdit(Vst::ParamID, Vst::ParamValue) override { return kResultOk; }
	tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }
	tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }

	// A host that grows its window when the plugin asks. The crash path does not
	// use it, but a view that calls it and gets kResultFalse may take a different
	// branch, so answer properly.
	tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override
	{
		std::printf("  [host] resizeView requested: %d x %d\n",
			newSize->right - newSize->left, newSize->bottom - newSize->top);
		return kResultOk;
	}
};

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_CLOSE)
		return 0;               // the harness decides when to finish, not the user
	return DefWindowProc(h, m, w, l);
}

// Dispatch messages for ms milliseconds. The editor's Direct2D device is created
// lazily on the first paint, so the crash path is not even reachable until this
// has run at least once.
void pump(int ms)
{
	const DWORD until = GetTickCount() + DWORD(ms);
	MSG msg;
	while (GetTickCount() < until)
	{
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		Sleep(5);
	}
}

// The size that matters is the PLUGIN's window, not ours. attached() creates a
// child window inside the HWND we hand over, and DrawingFrame::reSize calls
// SetWindowPos on that child -- our own client rect never moves. P2 measured the
// wrong window here and concluded "MoveWindow does not resize it".
HWND pluginChild(HWND parent)
{
	return GetWindow(parent, GW_CHILD);
}

void clientSize(HWND h, int& w, int& hgt)
{
	RECT r{};
	if (!h || !GetClientRect(h, &r))
	{
		w = hgt = -1;
		return;
	}
	w = r.right - r.left;
	hgt = r.bottom - r.top;
}

} // anonymous namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <plugin.vst3> [right] [bottom]\n", argv[0]);
		return 1;
	}

	const char* pluginPath = argv[1];
	// Defaults are the rect the crash dump recorded REAPER passing.
	const int crashRight  = (argc > 2) ? std::atoi(argv[2]) : 2178;
	const int crashBottom = (argc > 3) ? std::atoi(argv[3]) : 32672;

	std::printf("plugin: %s\n", pluginPath);

	HMODULE dll = LoadLibraryA(pluginPath);
	if (!dll)
	{
		std::fprintf(stderr, "FAIL: LoadLibrary failed, GetLastError=%lu\n", GetLastError());
		return 1;
	}

	// The Windows pair. (The Linux pair, ModuleEntry/ModuleExit, is X3's problem.)
	if (auto initDll = reinterpret_cast<bool (PLUGIN_API*)()>(GetProcAddress(dll, "InitDll")))
		initDll();

	auto getFactory = reinterpret_cast<IPluginFactory* (PLUGIN_API*)()>(GetProcAddress(dll, "GetPluginFactory"));
	if (!getFactory)
	{
		std::fprintf(stderr, "FAIL: no GetPluginFactory export\n");
		return 1;
	}

	IPluginFactory* factory = getFactory();
	if (!factory)
	{
		std::fprintf(stderr, "FAIL: GetPluginFactory returned null\n");
		return 1;
	}

	Host host;

	// A factory may want the host context before anything is created.
	IPluginFactory3* factory3 = nullptr;
	if (factory->queryInterface(IPluginFactory3_iid, reinterpret_cast<void**>(&factory3)) == kResultOk && factory3)
		factory3->setHostContext(static_cast<Vst::IHostApplication*>(&host));

	// Find the audio effect class.
	Vst::IComponent* component = nullptr;
	PClassInfo chosen{};
	for (int32 i = 0; i < factory->countClasses(); ++i)
	{
		PClassInfo ci{};
		if (factory->getClassInfo(i, &ci) != kResultOk)
			continue;
		if (std::strcmp(ci.category, kVstAudioEffectClass) != 0)
			continue;
		if (factory->createInstance(ci.cid, Vst::IComponent_iid, reinterpret_cast<void**>(&component)) == kResultOk && component)
		{
			chosen = ci;
			break;
		}
	}

	if (!component)
	{
		std::fprintf(stderr, "FAIL: no audio effect class could be instantiated\n");
		return 1;
	}
	std::printf("class:  \"%s\"\n", chosen.name);

	if (component->initialize(static_cast<Vst::IHostApplication*>(&host)) != kResultOk)
	{
		std::fprintf(stderr, "FAIL: IComponent::initialize failed\n");
		return 1;
	}

	// Controller: separate class if the plugin names one, otherwise the same
	// object. TIDE is the separate-class shape, but do not assume it.
	Vst::IEditController* controller = nullptr;
	TUID controllerCid{};
	if (component->getControllerClassId(controllerCid) == kResultOk)
	{
		if (factory->createInstance(controllerCid, Vst::IEditController_iid, reinterpret_cast<void**>(&controller)) == kResultOk && controller)
			controller->initialize(static_cast<Vst::IHostApplication*>(&host));
	}
	if (!controller)
		component->queryInterface(Vst::IEditController_iid, reinterpret_cast<void**>(&controller));

	if (!controller)
	{
		std::fprintf(stderr, "FAIL: no edit controller\n");
		return 1;
	}
	controller->setComponentHandler(static_cast<Vst::IComponentHandler*>(&host));

	IPlugView* view = controller->createView(Vst::ViewType::kEditor);
	if (!view)
	{
		std::fprintf(stderr, "FAIL: createView returned null -- no editor to resize\n");
		return 1;
	}

	if (view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue)
	{
		std::fprintf(stderr, "FAIL: view does not support HWND\n");
		return 1;
	}

	ViewRect vs{};
	view->getSize(&vs);
	const int w0 = (vs.right - vs.left) > 0 ? (vs.right - vs.left) : 800;
	const int h0 = (vs.bottom - vs.top) > 0 ? (vs.bottom - vs.top) : 600;
	std::printf("view size: %d x %d\n", w0, h0);

	WNDCLASSA wc{};
	wc.lpfnWndProc = wndProc;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.lpszClassName = "TideResizeHost";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassA(&wc);

	RECT want{ 0, 0, w0, h0 };
	AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowExA(0, "TideResizeHost", "TIDE editor resize harness",
		WS_OVERLAPPEDWINDOW, 100, 100,
		want.right - want.left, want.bottom - want.top,
		nullptr, nullptr, wc.hInstance, nullptr);
	if (!hwnd)
	{
		std::fprintf(stderr, "FAIL: CreateWindow failed\n");
		return 1;
	}
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	// setFrame before attached: that is where a view is meant to pick up its frame.
	view->setFrame(static_cast<IPlugFrame*>(&host));

	if (view->attached(hwnd, kPlatformTypeHWND) != kResultTrue)
	{
		std::fprintf(stderr, "FAIL: attached() failed\n");
		return 1;
	}
	std::printf("attached: ok\n");

	// Let it paint. Until the first paint there is no Direct2D device, and with
	// no device the pre-fix reSize returns at its own first test -- which would
	// look exactly like the fix working.
	pump(900);

	// ---------------------------------------------------------------------
	// Liveness probe. A benign resize that the window actually adopts proves
	// reSize got past `if (d2dDeviceContext && ...)` and called SetWindowPos --
	// i.e. the device is live and the crash path is genuinely reachable.
	// Without this, "it survived" is indistinguishable from "it never ran".
	// ---------------------------------------------------------------------
	HWND child = pluginChild(hwnd);
	if (!child)
	{
		std::fprintf(stderr, "FAIL: attached() created no child window to measure\n");
		return 1;
	}
	std::printf("plugin child window: %p\n", (void*)child);

	int cw = 0, ch = 0;
	clientSize(child, cw, ch);
	std::printf("child before probe: %d x %d\n", cw, ch);

	const int probeW = (cw > 500) ? cw - 120 : cw + 120;
	const int probeH = (ch > 400) ? ch - 90  : ch + 90;
	std::printf("probe onSize(0, 0, %d, %d)\n", probeW, probeH);

	ViewRect probe{ 0, 0, probeW, probeH };
	view->onSize(&probe);
	pump(400);

	int pw = 0, ph = 0;
	clientSize(child, pw, ph);
	std::printf("child after probe:  %d x %d\n", pw, ph);

	const bool live = (pw == probeW && ph == probeH);
	std::printf("editor live (device present, SetWindowPos took effect): %s\n", live ? "YES" : "NO");

	// ---------------------------------------------------------------------
	// P4b lives on a different call than the crash. checkSizeConstraint is what
	// a *polite* host asks before calling onSize; the VST3 contract is that it
	// writes back the nearest size the view will take. Returning kResultFalse
	// with the rect untouched tells the host nothing, which is how an
	// unnegotiated rect reaches onSize in the first place. Probe it separately:
	// the crash path below bypasses it entirely.
	// ---------------------------------------------------------------------
	ViewRect ask{ 0, 0, crashRight, crashBottom };
	const tresult csc = view->checkSizeConstraint(&ask);
	std::printf("\ncheckSizeConstraint(0, 0, %d, %d) -> %s, rect now %d x %d %s\n",
		crashRight, crashBottom,
		(csc == kResultTrue) ? "kResultTrue" : "kResultFalse",
		ask.right - ask.left, ask.bottom - ask.top,
		(ask.right == crashRight && ask.bottom == crashBottom)
			? "(UNCHANGED -- host learns nothing)" : "(adjusted -- host has a usable answer)");

	// ---------------------------------------------------------------------
	// The crash input, verbatim from the minidump.
	// ---------------------------------------------------------------------
	std::printf("\n>>> onSize(0, 0, %d, %d)  -- the rect from the crash dump\n", crashRight, crashBottom);
	std::fflush(stdout);

	ViewRect crash{ 0, 0, crashRight, crashBottom };
	view->onSize(&crash);

	std::printf("<<< SURVIVED onSize\n");
	std::fflush(stdout);

	pump(300);
	clientSize(pluginChild(hwnd), cw, ch);
	std::printf("child after oversized onSize: %d x %d\n", cw, ch);

	view->removed();
	view->setFrame(nullptr);
	view->release();
	controller->terminate();
	controller->release();
	component->terminate();
	component->release();
	DestroyWindow(hwnd);

	if (!live)
	{
		std::fprintf(stderr, "\nINCONCLUSIVE: the editor was not provably live, so surviving proves nothing.\n");
		return 3;
	}

	std::printf("\nPASS: editor was live and the oversized onSize did not crash.\n");
	return 0;
}
