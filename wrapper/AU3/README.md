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

## Consuming: `FORMATS_LIST ... AU3`

`gmpi_plugin.cmake`'s AU3 arm does the whole dance from one word in the
format list, on macOS and iOS both: a `<Plugin>_AU3.appex` (plugin sources +
`wrapperAu3.mm`, linked against `AU3_Wrapper`, `-e _NSExtensionMain`), a
`<Plugin>_AU3App` containing app (`mac/HostAppMain.mm` / `ios/HostAppMain.mm`),
the appex `Info.plist` written by `plist_util --au3`, and an always-run
assemble target that nests the appex in the app's `PlugIns/` and ad-hoc signs
inside-out (with `appex.entitlements` on macOS — extensions must be sandboxed
there, and an unsandboxed one silently fails to load; iOS is sandboxed by
definition and takes no such key).

The plist derivation is the same one the AU2 `.component` gets, so v2 and v3
share their fourCCs — with one twist per platform: macOS scans the **built
module** (load it, ask its factory); an iOS-built module cannot be loaded by
the Mac doing the building, so there a **host-compiled** `plist_util` reads
the identical metadata from the plugin's own declaration (`--xml`, which
accepts a `.xml` or the source file holding a `Register<>::withXml` raw
string). The two modes emit byte-identical plists.

macOS: under `SE_LOCAL_BUILD` the app is copied to `~/Applications` and the
appex registered with `pluginkit -a`; validate with
`auval -v <type> <subtype> <manu>`.

iOS: configure with `-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator`
(or `iphoneos`); every other format drops out of the list there, the way AU
does on Windows. Then `xcrun simctl install booted <Plugin>_AU3App.app` and
launch it once — the extension registers and AUv3 hosts list it. Device
installs need a real signing identity in place of the assemble step's ad-hoc
signature; that re-sign is the consumer's, outside this build.

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
* iOS device signing (simulator builds are verified end-to-end; a device
  build needs the consumer's identity and provisioning in place of the
  ad-hoc signature).
