# @gmpi/standalone-mcp

A [Model Context Protocol](https://modelcontextprotocol.io) server that drives a
**running** GMPI standalone plugin app — read and set parameters, inject MIDI,
click and drag the GUI, screenshot the window, and render audio offline.

Unlike [`@synthedit/mcp`](https://www.npmjs.com/package/@synthedit/mcp), which
wraps a headless command-line tool, this one has no headless mode at all. Every
tool acts on the app the user actually has open, so a screenshot is *their*
window and a parameter change moves the knob in front of them. The thing being
tested is the real one.

## Requirements

- Node ≥ 18
- A GMPI standalone built from this repo, running. It prints
  `command channel: /run/user/1000/gmpi-standalone/gmpi-standalone.<pid>`
  at startup when the channel is open.
- Linux/Wayland — the standalone wrapper's only platform today.

## Build

```bash
npm install && npm run build
```

`dist/` is build output and is **not** committed, so a fresh clone has no server
until you run those two commands. The config below points straight at
`dist/index.js`, so skipping the build makes the tools quietly fail to appear
rather than reporting an error.

## Use it from Claude Code

The repo ships a `.mcp.json` at its root that registers this server
automatically — open the repo and approve `gmpi-standalone` when prompted (or
run `/mcp`). To register it from another project:

```json
{
  "mcpServers": {
    "gmpi-standalone": {
      "command": "node",
      "args": ["/path/to/GMPI_Wrappers/mcp/dist/index.js"]
    }
  }
}
```

No environment configuration is needed: the server finds running apps by
listing the runtime directory, and derives that path itself when
`XDG_RUNTIME_DIR` is absent (which it usually is under an MCP host — see below).

## Tools

| Tool | What |
|---|---|
| `gmpi_list_apps` | Running standalones, with pid and plugin name. Everything else targets the single running app automatically. |
| `gmpi_info` | Plugin name/id/vendor, channel counts, sample rate, and the window geometry needed to map DIPs ↔ screenshot pixels. |
| `gmpi_list_params` | Every parameter: id, name, datatype, min/max, and current value in both real and normalised form. |
| `gmpi_get_param` / `gmpi_set_param` | Read / write one parameter. A write updates the GUI **and** the audio processor. |
| `gmpi_screenshot` | PNG of the window as it appears on screen. |
| `gmpi_drag`, `gmpi_pointer` | Drive the GUI the way a mouse does. |
| `gmpi_note`, `gmpi_cc`, `gmpi_all_notes_off`, `gmpi_midi_raw` | Inject MIDI into the running instance. |
| `gmpi_render_audio` | Render to WAV offline and report peak / rms / clipping. |
| `gmpi_script` | Several verbs in one round-trip. |

## Two things worth knowing

**Screenshots need no compositor permission.** On Wayland every desktop
screenshot route goes through the compositor, and GNOME refuses both
`org.gnome.Shell.Screenshot` and the xdg-desktop-portal one to an unattended
caller. The app renders and reads its *own* buffer instead, so this works
headlessly, from a script, and over ssh.

**`gmpi_render_audio` answers "did it make the right sound" without reading the
file.** The result carries `peak`, `rms`, `clippedSamples` and a `silent` flag.
It runs on its own processor instance primed with the current parameter values,
so it neither disturbs nor is disturbed by whatever the app is playing, and it
works even when no audio device is open. Being deterministic, it is a
repeatable check rather than a recording of what you are hearing right now.

## How it finds the app

A directory listing *is* the discovery mechanism — the app publishes nothing
else, so there is no registry, config file or port to keep in sync. The socket
is named `gmpi-standalone.<pid>`, mode 0700. Directories tried, in order:

1. `$GMPI_STANDALONE_IPC_DIR` — overrides everything, for tests.
2. `$XDG_RUNTIME_DIR/gmpi-standalone`
3. `/run/user/<uid>/gmpi-standalone` — the same path, derived rather than read.
   Not redundant: an MCP host does not hand its servers the user's whole
   environment, so `XDG_RUNTIME_DIR` is typically *absent* here even though the
   app that published the socket had it.
4. `/tmp/gmpi-standalone.<uid>` — for a bare ssh session or container, where
   the pam module that creates `XDG_RUNTIME_DIR` never ran.

Entries whose pid is gone are pruned: a unix socket file does not evaporate
with its process, so a crashed app leaves a corpse behind, and without pruning
"2 apps are running — pass pid to choose one" fires against ghosts.

## Test

With a standalone running:

```bash
npm run build && node test/smoke.mjs
```

## License

ISC — see [LICENSE](../LICENSE) at the repo root, the same terms GMPI,
gmpi_ui and GMPI_Adaptors carry.
