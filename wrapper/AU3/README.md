# AudioUnit V3 (AUv3) wrapper — macOS and iOS

Wraps a GMPI plugin as an `AUAudioUnit` inside an app extension (`.appex`).
This is the only plugin format iOS supports, and on macOS it loads
out-of-process beside the AU2 `.component` this repo also builds.

## What the static library provides

| piece | role |
|---|---|
| `GmpiAudioUnit` (AU3_Wrapper.mm) | `AUAudioUnit` subclass: busses, parameter tree, render block, MIDI 1.0 + 2.0 (UMP), tempo/musical-time host controls, `fullState` with the same `GMPIPRESET` chunk the AU2 wrapper writes |
| `GmpiAUViewController` (AU3_ViewController.mm) | the extension's principal class (`AUAudioUnitFactory`); creates the unit on request and hosts the GMPI editor — NSView on macOS, UIView on iOS via gmpi_ui's `createNativeView` |
| `wrapperAu3.mm` | compiled into the consuming appex so the linker keeps both classes (the principal class is looked up by name, never by symbol) |
| `appex-Info.plist.in` | template for the appex's `Info.plist` (`configure_file` `@ONLY`) |

No AudioUnitSDK checkout is needed — `AUAudioUnit` is AudioToolbox API.

## Consuming: anatomy of the .appex

An AUv3 ships inside a **containing app**; the extension is
`YourApp.app/Contents/PlugIns/YourPlugin.appex` (`PlugIns/` on iOS too).

1. **appex target**: an executable bundle with `BUNDLE_EXTENSION "appex"`
   compiling the plugin's own sources (the same list every other format
   target compiles, so `MP_GetFactory` resolves) plus `wrapperAu3.mm`,
   linking `AU3_Wrapper`. Link flags: `-e _NSExtensionMain`.
2. **Info.plist** from `appex-Info.plist.in`. The identity fields (`type`,
   `subtype`, `manufacturer`, version integer) must match what plist_util
   derives for the AU2 component from the plugin's XML — same fourCCs, so the
   v2 and v3 releases of one plugin describe the same product.
3. **Sign** the appex **with the app-sandbox entitlement** (extensions must be
   sandboxed; an unsandboxed one silently fails to load), then sign the app.
4. macOS: registration happens when the app first runs (or `pluginkit -a
   path/to/.appex` during development); validate with
   `auval -v <type> <subtype> <manu>`. iOS: install the containing app; hosts
   (GarageBand, AUM, Logic for iPad) list the extension.

A complete working reference — appex + containing app + ad-hoc signing +
`auval` — was built as a scratch rig; its CMakeLists is small and worth
copying when adding an AU3 format to `gmpi_plugin.cmake`.

## Design notes

* **Thread split** is the AU2 wrapper's, made stricter: `plugin` (processor
  store + event list) belongs to the render thread, `gmpiController` to the
  main thread, and *every* main-thread parameter change crosses through the
  `queueToDsp` inter-thread queue ("ppc2"/"ppc3" frames) rather than touching
  render-side structures directly. Sample-accurate host automation arrives as
  `AURenderEventParameter` on the render thread and goes straight into the
  event list.
* **No `implementorValueProvider`.** The observer may defer its store update
  to the main thread; a host that sets a value and reads it straight back
  (auval; Ableton) must see the value it just set, which `AUParameter`'s own
  cache guarantees. Editor-originated edits reach that cache through
  `notifyDaw` → `-[AUParameter setValue:originator:]`.
* **The editor is created and measured in `loadView`,** before the
  view-bridge delivers the view, because `preferredContentSize` set after
  delivery never reaches the host's proxy. Wiring to the unit
  (`createNativeView` + `initUi`) waits until both halves exist.
* **iOS UI** comes from gmpi_ui's `DrawingFrameIos.mm` — same CoreGraphics
  backend and linear-colorspace backing bitmap as the Mac frame, touch input
  mapped to GMPI pointer events, UIAlertController dialogs (popup menu, text
  edit, stock dialogs; file/color dialogs decline with `NoSupport`).

## Not done yet

* Stereo-pair bus splitting for Logic-style multi-out instruments (one bus
  per side carries all channels today; AU2 splits pairs).
* Factory presets (`factoryPresets` returns nothing; `fullState` works).
* An `AU3` format arm in `gmpi_plugin.cmake` to generate the appex +
  containing-app targets from the plugin XML the way the other formats do.
* iOS runtime testing (the library and view host compile for device and
  simulator; a containing iOS app has not been assembled here).
