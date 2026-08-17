# GMPI standalone MCP server — agent reference

A stdio MCP server that drives a **running** GMPI standalone plugin app over a
unix socket. The app-side half lives in
[../wrapper/Standalone/mcp/](../wrapper/Standalone/mcp/) and is the source of
truth for behaviour; this package is a thin wrapper over its verbs.

## TL;DR

```bash
cd <repo>/mcp
npm install && npm run build     # dist/ is NOT committed
```

Launch a standalone (it prints `command channel: <path>`), then the `gmpi_*`
tools work. No env vars needed.

## The one structural difference from `@synthedit/mcp`

**There is no headless mode, and no session to lose.** SynthEdit's server owns a
long-lived `SynthEditCL` child holding a document, so it needs `se_reset`,
`sessionLost`, and careful respawn logic. Here the *app* owns everything —
parameters, GUI, audio — and it keeps running whether or not anyone is
connected. So:

- Every tool opens a connection, sends its verbs, and closes. There is no
  persistent socket, because a persistent one would buy nothing but a reconnect
  path to get wrong. (An idle client holding a connection open is exactly what
  made SynthEdit's editor *look* wedged; see `kMaxClients` in `IpcServer.h`.)
- A dropped connection costs nothing. Only the app exiting is real loss, and
  that reports as "no running app" rather than a session error.
- State accumulates in the app across tool calls for free: set three
  parameters in three calls, then screenshot, and the screenshot shows all
  three.

`gmpi_script` exists purely to batch several verbs into one round-trip, never
because statefulness requires it.

## Coordinates

Three spaces, and conflating them is the easiest mistake to make:

| Space | What | Where reported |
|---|---|---|
| **Logical DIPs** | What `gmpi_pointer` / `gmpi_drag` take. Origin is the top-left of the **window**. | `windowWidth` / `windowHeight` |
| **Pixels** | What a screenshot is measured in. | `canvasWidth` / `canvasHeight` |
| **Plugin-relative** | What the plugin's own code thinks in. | add/subtract `editorOriginY` |

`scale` = canvas ÷ window. At scale 1 (no fractional scaling) DIPs and pixels
are the same number, so you can read a coordinate straight off the PNG — which
is why the pointer verbs take window coordinates rather than plugin-relative
ones. The app's menu bar occupies the strip above `editorOriginY`; a plugin
coordinate `(x, y)` is window `(x, y + editorOriginY)`.

## Instruments vs effects

The single most common way to get a useless answer: exciting the plugin the
wrong way. Check `audioInputs` in `gmpi_info` first.

| | How you excite it | What "no excitation" proves |
|---|---|---|
| **Instrument** (`audioInputs: 0`) | `note` | a synth rendering `silent:true` with no note is healthy |
| **Effect** (`audioInputs > 0`) | `input: "tone"` or `"noise"` | an effect rendering silence with silence in is healthy |

An effect fed the default silence outputs silence — `ok:true`, `silent:true`,
and nothing learned. Passing `note` to an effect does nothing at all; passing
`input` to a plugin with no audio inputs is refused rather than ignored, so you
find out immediately.

Both generators are deterministic (fixed-seed LCG for noise, phase carried
across blocks for the tone), so two runs of the same command produce identical
files and a difference is a real regression.

**Gain checks are arithmetic.** The result echoes `inputLevel`, so:

```text
gmpi_render_audio input="tone" inputLevel=0.5   → peak 0.5   ⇒ unity gain
                                                → peak 1.0   ⇒ 2x
                                                → peak 0.25  ⇒ 0.5x
```

An `rms` of exactly `peak / √2` additionally tells you the output is a clean
sine — i.e. the plugin applied gain without distorting or clipping.

## Restoring state

`gmpi_list_params` reports `default` alongside `value`. Put a parameter back to
its **default**, not to whatever it was on entry: the incoming value may itself
be the degenerate leftover of the last thing that ran, and a gain sitting at 0
makes a perfectly healthy plugin look broken.

## Workflow: "check the filter actually filters"

```text
1. gmpi_list_params                    → find "Cutoff in Keys" is id 7, range 1..127
2. gmpi_render_audio path=/tmp/open.wav   note=60 seconds=1     → peak 0.24
3. gmpi_set_param id=7 value=20
4. gmpi_render_audio path=/tmp/closed.wav note=60 seconds=1     → peak much lower
5. gmpi_screenshot path=/tmp/after.png                          → the slider moved
```

Steps 2 and 4 do not need the WAVs opened at all — `peak`, `rms` and `silent`
in the result are the comparison. Open them only when you need the spectrum or
the envelope.

**Render a control case.** A render with no `note` should come back
`silent:true` for a synth. If it does not, something is self-oscillating or
stuck, and every other measurement is suspect.

## Driving the GUI

`gmpi_drag` is one round-trip for the whole gesture — press, N moves, release —
because it runs inside a single visit to the app's main thread. Prefer it over
three `gmpi_pointer` calls, which cost a tick each and let the app repaint
mid-gesture in a way no real mouse produces.

Events enter at the same `IInputClient` the Wayland seat delivers to, so mouse
capture and hover work normally: a drag that leaves the control keeps going,
exactly as under a hand.

**A drag reports the gesture, not the outcome.** Check what it did with
`gmpi_get_param`. How far a knob moves per pixel is the plugin's business — the
SawDemo editor uses 0.005 normalised units per pixel, others differ.

## MIDI

`gmpi_note` injects at the point a real MIDI cable enters, so it goes through
the plugin's normal path and you HEAR it if audio is running. It is for
exercising the live instance.

For a note you can *verify*, use `gmpi_render_audio` with `note` — it plays the
note offline and reports the level. That is a measurement; `gmpi_note` is a
poke.

Channels are 1-16 as a musician counts them, not 0-15 as the wire encodes them.

**Injected notes can hang.** A `gmpi_note action=on` with no matching `off`
leaves the voice sounding forever. `gmpi_all_notes_off` is the fix.

## Failure modes worth recognising

| Symptom | Meaning |
|---|---|
| `No running GMPI standalone found` | Nothing launched, or it failed to open its channel — check its stderr for the `command channel:` line. |
| `N standalones are running` | Pass `pid`, from `gmpi_list_apps`. |
| `busy: true` | The app's main thread is blocked (a modal dialog, or a wedged plugin). The command was **not** run — retry once it is responsive. |
| `stale socket behind` | The app crashed without unlinking. Its next launch cleans up. |
| `this plugin has no MIDI input pin` | Not an error in the tool — the plugin is an effect, not an instrument. |

## Files

- [src/index.ts](src/index.ts) — tool registration, one tool per verb plus `gmpi_script`.
- [src/discover.ts](src/discover.ts) — finds running apps by directory listing; prunes dead pids.
- [src/session.ts](src/session.ts) — connect, `--ping` framing, app selection.
- [test/smoke.mjs](test/smoke.mjs) — end-to-end over real MCP stdio. Needs a running app.

## App-side notes

The verbs, the socket and the threading contract are documented where they
live:

- [../wrapper/Standalone/mcp/IpcServer.h](../wrapper/Standalone/mcp/IpcServer.h) — socket, framing, shutdown ordering.
- [../wrapper/Standalone/mcp/MainThreadQueue.h](../wrapper/Standalone/mcp/MainThreadQueue.h) — why the event-loop tick is the marshaller on Wayland.
- [../wrapper/Standalone/mcp/CommandDispatcher.cpp](../wrapper/Standalone/mcp/CommandDispatcher.cpp) — every verb.

Adding a verb: implement it in `CommandDispatcher.cpp`, then either expose it as
a tool in `src/index.ts` or reach it through `gmpi_script` — the latter needs no
change here at all.
