# Standalone

A bare application that wraps one GMPI plugin: a window, a menu bar, an audio
device and a MIDI port. The equivalent of JUCE's standalone target — what you
build when you want to run a plugin without a DAW.

It is a wrapper like the others here (`VST3/`, `CLAP/`, `AU2/`) and works the
same way: a static library the plugin's own target links, with the plugin
statically linked in beside it. The plugin cannot tell the difference between
this and a DAW — it still gets a Processor subtype driven by `process()`, an
Editor subtype attached to a drawing host, and the same two inter-thread queues
carrying parameter changes between them.

## Using it

Add `STANDALONE` to a plugin's format list:

```cmake
gmpi_plugin(
    PROJECT_NAME ${PROJECT_NAME}
    HAS_DSP HAS_GUI
    FORMATS_LIST GMPI VST3 CLAP STANDALONE
    SOURCE_FILES ...
)
```

which produces a `<PluginName>_STANDALONE` executable next to the plugin
bundles. `GMPI-plugins/plugins/SawDemo` (a synth: MIDI in, stereo out) and
`GMPI-plugins/plugins/GainGui` (an effect: stereo in and out) both build one.

## Platform status

| | Window | Audio | MIDI in | Command channel |
| --- | --- | --- | --- | --- |
| Linux | Wayland (gmpi_ui's own backend, CPU rendering) | PipeWire | ALSA sequencer | unix socket |
| Windows | Win32 + Direct2D (gmpi_ui's `DrawingFrame`, the one the VST3 wrapper embeds) | WASAPI, shared mode | winmm | named pipe |
| macOS | not written | not written | not written | not written |

`gmpi_plugin()` drops `STANDALONE` from the format list on the platforms that
have no shell, with a `message(STATUS)` saying so, and this directory's
`CMakeLists.txt` returns immediately. A cross-platform project can therefore
list `STANDALONE` unconditionally.

The command channel is compiled in by **default** and can be switched off with
the CMake option `GMPI_STANDALONE_COMMAND_CHANNEL=OFF` (or
`-DGMPI_STANDALONE_COMMAND_CHANNEL=0` straight to the compiler). Off removes
the code rather than merely declining to start it — `mcp/` is not compiled at
all — so a vendor who does not want a local IPC endpoint in a signed product
has nothing left to audit. See `CommandChannel.h`.

## Layout

Portable — no window-system headers, shared by every platform:

| File | |
| --- | --- |
| `StandaloneHost.*` | the plugin: factory → processor + controller + editor, the audio callback, the MIDI FIFO |
| `AudioMidiDevices.h` | the `AudioDriver` / `MidiDriver` seam the shells implement |
| `StandaloneSettings.*` | persisted device selection (`~/.config/<plugin>/standalone.conf` and its Windows/macOS equivalents) |
| `AppLayout.*` | the window's root: menu bar strip + one of several content pages |
| `MenuBarView.*` | the menu bar, drawn (gmpi_ui, no toolkit) |
| `SettingsPane.*` | the Audio/MIDI page, a `gmpi::ui::Form` |

Per platform:

| File | |
| --- | --- |
| `linux/MainWayland.cpp` | the entry point: connection, window, event loop |
| `linux/AudioDriverPipeWire.*` | playback + capture streams, device enumeration |
| `linux/MidiDriverAlsa.*` | sequencer input |
| `windows/MainWin32.cpp` | the entry point: DPI awareness, COM, the message loop |
| `windows/ToplevelWindow.*` | the overlapped window gmpi_ui's `DrawingFrame` lives inside |
| `windows/AudioDriverWasapi.*` | render + capture streams, device enumeration |
| `windows/MidiDriverWin.*` | winmm input, including sysex |
| `windows/FrameCapture.*` | `--screenshot`: reads the window back out of the swap chain |

`compat/it_enum_list.h` is a shim, not a component — see the comment at the top
of it.

The Linux drivers are adapted from SynthEdit's Wayland editor
(`SE16/SynthEditWayland/IO_PipeWire.*` and `MidiDriverAlsa.*`), which is where
the stream setup, the negotiation wait, the capture ring buffer and the
non-blocking sequencer handle were worked out. What changed is the far end:
they call an `AudioCallback` instead of driving `UIoManager`, so this wrapper
depends on GMPI and gmpi_ui only, never on SynthEditLib.

## The Windows shell

Much thinner than the Linux one, because gmpi_ui already ships the hard half.
`gmpi::hosting::DrawingFrame` is the Direct2D editor frame the GMPI **VST3
wrapper** embeds in a DAW: swap chain, render loop, mouse and keyboard
dispatch, tooltips, and native popup menus, text edits and file dialogs. What
it cannot do is exist on its own — `open()` takes a parent and creates a
`WS_CHILD` window inside it — so `windows/ToplevelWindow.*` supplies exactly
that missing parent and nothing else.

A consequence worth stating: the plugin sees the identical arrangement it sees
in a DAW. Its editor is in a child HWND owned by someone else, at a DPI it must
read from the window rather than assume. Anything that only worked because the
frame happened to be top-level would be a bug this shell would have hidden.

The menu bar is **drawn**, not a native `HMENU`, so the app's chrome matches the
Linux one. The drop-downs themselves are native: `MenuBarView` opens them
through `IDialogHost::createPopupMenu`, which on Windows is `TrackPopupMenu`.

Two WASAPI details that surprise people:

- **The buffer size in settings is a request, not a setting.** In shared mode
  the engine owns the buffer and hands back its own size — asking for 512
  frames typically yields 1056. `getBufferFrames()` reports what was granted,
  and `StandaloneHost` starts the processor against that, so the status line on
  the settings page reads e.g. `Running: 48000 Hz, 1056 frames`.
- **A non-native sample rate goes through the engine's resampler**
  (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`) rather than reclocking the device,
  which is what lets a plugin be auditioned at the rate its presets were made
  at. A device that refuses those flags falls back to its own mix format, and
  the rate actually running is again reported rather than assumed.

COM apartments are the other thing to keep straight: every WASAPI object is
created **on the thread that uses it**, each device thread being an MTA of its
own. `open()` starts the render thread and waits for it to report back rather
than activating an `IAudioClient` on the UI thread (an STA, because the window
needs OLE) and calling it from elsewhere.

## Building on Linux

Needs a reasonably current distro — Ubuntu 24.04 or later, matching what
SynthEdit's own Linux CI runs on. Ubuntu 22.04 is **too old**: gmpi_ui's CPU
text engine needs HarfBuzz 4+ and its Wayland backend needs libwayland 1.22+
and wayland-protocols 1.32+.

```
sudo apt-get install -y \
  libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev \
  libdbus-1-dev libfreetype-dev libfontconfig1-dev libharfbuzz-dev libpng-dev \
  libpipewire-0.3-dev libasound2-dev
```

Install `libdecor-0-plugin-1-gtk` too, or libdecor finds no plugin and the
window has no title bar or border.

`GMPI_WAYLAND_PROTOCOLS_DIR` (a CMake cache variable) is searched ahead of the
system `wayland-protocols` tree, for a build host whose packages are older than
the staging protocols the backend binds.

## Behaviour worth knowing

- **Audio failing to open is not fatal.** The app comes up on the settings page
  with the error on it, rather than refusing to start — the settings page is
  the only thing that can fix a busy or missing device.
- **An empty MIDI input list means "connect everything readable"**, so a fresh
  install plays as soon as a keyboard is plugged in. Once the user has visited
  the settings page, the saved list is honoured exactly, including empty.
- **Settings apply instantly, with no OK button**, and the apply is deferred to
  the next frame — re-opening an audio device inside a click's event dispatch
  would join the driver's threads with the compositor waiting on us.

## The command channel

Every standalone opens an IPC endpoint that drives the plugin it is hosting —
read and set parameters, inject MIDI, click and drag the GUI, screenshot the
window, render audio offline. It exists to make plugin testing scriptable: the
thing being driven is the app the user actually has open, not a headless second
copy of it.

**Discovery is a directory listing** on both platforms — no registry, config
file or port to keep in sync. The leaf name carries the pid, because that is
the only identifier available before connecting.

| | Published at | Access |
| --- | --- | --- |
| Linux | `$XDG_RUNTIME_DIR/gmpi-standalone/gmpi-standalone.<pid>`, falling back to `/tmp/gmpi-standalone.<uid>/` | the socket is mode 0700 in a directory whose ownership is checked |
| Windows | `\\.\pipe\gmpi-standalone.<pid>` — the pipe namespace enumerates like any other directory | the pipe's default DACL, plus `PIPE_REJECT_REMOTE_CLIENTS` |

`GMPI_STANDALONE_IPC_DIR` overrides the Linux location, for tests; Windows has
one namespace and needs no equivalent. Failing to open the channel is never
fatal — the app runs normally without one.

The grammar is SynthEditCL's: newline-framed shell-style verb lines in, one
JSON object per line out, so anything that speaks a socket or a pipe can drive
it —

```bash
printf -- '--info\n--set-param 7 30\n--screenshot /tmp/a.png\n' \
  | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/gmpi-standalone/gmpi-standalone.10673
```

— and [../../mcp/](../../mcp/) wraps the same verbs as MCP tools for an AI
agent. Node's `net.createConnection` takes a pipe path as happily as a socket
path, so the MCP server differs between the two only in which directory it
lists.

### Two things it can do that the desktop cannot

- **Screenshot itself**, with no compositor, portal or permission involved. On
  Linux that matters because every desktop route to a screenshot goes through
  the compositor, and GNOME refuses both `org.gnome.Shell.Screenshot` and the
  xdg-desktop-portal one to an unattended caller; the app reads its own shm
  buffer instead. Windows has no such restriction, but reading the swap chain
  back (`windows/FrameCapture.*`) still beats `PrintWindow` from outside: it
  captures the client area exactly, at real pixel dimensions, without the
  caller having to be DPI-aware to get an unclipped image. Both platforms emit
  the same BGRX8888 buffer, so one PNG writer and one MCP client serve both.
- **Render audio offline**, on a processor of its own primed with the current
  parameter values — faster than realtime, without disturbing what is playing,
  and even when no audio device would open. The result reports peak, rms and
  clipping, so "did it make the right sound" is answerable without opening the
  WAV.

### Implementation notes

| File | What |
|---|---|
| `mcp/IpcServer.h` | The socket: publishing, framing, client handling, shutdown ordering. |
| `mcp/MainThreadQueue.h` | Why the event loop's tick is the only usable main-thread marshaller here. |
| `mcp/CommandDispatcher.cpp` | Every verb. |

**Commands run on the main thread, once per frame.** The listener thread never
touches the plugin: it parses a line, hands it to `MainThreadQueue`, and blocks
until the tick has run it. Windows and macOS have `DispatcherQueue::TryEnqueue`
and `dispatch_async` for this; a Wayland app has neither, and its main thread is
parked in `poll()` inside `runEventLoop` — so the loop's tick *is* the queue.
That missing marshaller is why SynthEdit's live transport was never ported to
Linux.

**Pointer verbs enter at the same `IInputClient` the seat delivers to**, so
capture and hover behave exactly as under a real mouse, and coordinates are the
window's logical DIPs — the space a screenshot is measured in at scale 1.
