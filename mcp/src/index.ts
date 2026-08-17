#!/usr/bin/env node
/**
 * GMPI standalone MCP server — drives a RUNNING standalone plugin app over its
 * unix command channel (see wrapper/Standalone/mcp/ in this repo).
 *
 * There is no headless mode and no second copy of the plugin: every tool acts
 * on the app the user actually has open, so a screenshot is their window and a
 * parameter change moves the knob in front of them. That is the whole point -
 * the thing being tested is the real one.
 *
 * The app is the source of truth for behaviour and arguments; this file is a
 * thin wrapper over its verbs. Each tool call opens a connection, sends its
 * verbs framed by `--ping`, and closes - the app holds the state, not us.
 */

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { discoverApps } from "./discover.js";
import { run, resolveApp, call, NoAppError } from "./session.js";

const server = new McpServer({
  name: "gmpi-standalone",
  version: "0.1.0",
});

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Quotes a token for the app's shell-style line grammar, if it needs it. */
function quote(value: string): string {
  return /[\s"\\]/.test(value) ? `"${value.replace(/([\\"])/g, "\\$1")}"` : value;
}

function ok(payload: unknown, isError = false) {
  return {
    content: [{ type: "text" as const, text: JSON.stringify(payload) }],
    isError,
  };
}

/**
 * Runs verbs and returns the single result line.
 *
 * A failure to reach the app at all is reported as an error result rather than
 * thrown, so the model sees the explanation (and the "launch one and retry"
 * hint) instead of a bare transport stack trace.
 */
async function one(commands: string[], pid?: number) {
  try {
    const { lines, app } = await run(commands, pid);
    const line = lines[0] ?? { ok: false, error: "the app returned nothing" };
    return ok({ ...line, pid: app.pid }, line.ok === false);
  } catch (e: any) {
    return ok({ ok: false, error: e?.message ?? String(e) }, true);
  }
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

server.registerTool(
  "gmpi_list_apps",
  {
    description:
      "List the running GMPI standalone apps that have a command channel open, with their pid and plugin name. " +
      "Every other tool targets the single running app automatically; pass its pid only when more than one is running. " +
      "An app appears here from the moment it prints 'command channel: <path>' at startup.",
    inputSchema: {},
  },
  async () => {
    const apps = discoverApps();
    if (apps.length === 0)
      return ok({ ok: true, count: 0, apps: [], hint: "No standalone is running. Launch one and call this again." });

    // Ask each one what it is: a pid alone does not tell the user which plugin
    // they are looking at, and that is the only thing they actually care about
    // when choosing between two.
    const described = await Promise.all(apps.map(async app => {
      try {
        const { lines } = await call(app, ["--info"]);
        const info = lines[0] ?? {};
        return { pid: app.pid, path: app.path, name: info.name, audioRunning: info.audioRunning };
      } catch (e: any) {
        return { pid: app.pid, path: app.path, unreachable: e?.message ?? String(e) };
      }
    }));

    return ok({ ok: true, count: described.length, apps: described });
  },
);

const pidArg = z.number().int().optional()
  .describe("Which app to target. Omit when exactly one standalone is running (the usual case).");

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

server.registerTool(
  "gmpi_info",
  {
    description:
      "Describe the running plugin: name, id, vendor, audio channel counts, whether it takes MIDI, whether audio is running, and the device's sample rate. " +
      "Also reports the window geometry needed to map between spaces: windowWidth/windowHeight in logical DIPs (what the pointer tools take), canvasWidth/canvasHeight in pixels (what a screenshot is), their ratio as `scale`, and `editorOriginY` — the height of the app's menu bar, i.e. how far down the window the plugin's own editor starts.",
    inputSchema: { pid: pidArg },
  },
  async ({ pid }) => one(["--info"], pid),
);

server.registerTool(
  "gmpi_list_params",
  {
    description:
      "List every parameter with its id, name, datatype, min/max, and current value in BOTH real and normalised (0..1) form. " +
      "Read this first to discover parameter ids. Parameters marked `private:true` are host-controls (tempo, song position) that the plugin reads rather than a user setting.",
    inputSchema: { pid: pidArg },
  },
  async ({ pid }) => one(["--list-params"], pid),
);

server.registerTool(
  "gmpi_get_param",
  {
    description: "Read one parameter's current value, in both real and normalised form.",
    inputSchema: {
      id: z.number().int().describe("Parameter id, from gmpi_list_params."),
      pid: pidArg,
    },
  },
  async ({ id, pid }) => one([`--get-param ${id}`], pid),
);

server.registerTool(
  "gmpi_set_param",
  {
    description:
      "Set a parameter, updating BOTH the GUI the user is looking at and the running audio processor. " +
      "Values are in the parameter's own units by default; set normalised:true to pass 0..1 instead. " +
      "Reports changed:false when the value already matched — the store short-circuits an unchanged write, so that means 'already there', not 'rejected'.",
    inputSchema: {
      id: z.number().int().describe("Parameter id, from gmpi_list_params."),
      value: z.number().describe("New value, in the parameter's units unless normalised is true."),
      normalised: z.boolean().optional().describe("Treat value as 0..1 across the parameter's range."),
      pid: pidArg,
    },
  },
  async ({ id, value, normalised, pid }) =>
    one([`--set-param ${id} ${value}${normalised ? " --normalised" : ""}`], pid),
);

// ---------------------------------------------------------------------------
// Seeing it
// ---------------------------------------------------------------------------

server.registerTool(
  "gmpi_screenshot",
  {
    description:
      "Save a PNG of the app's window exactly as it appears on screen, including the menu bar. " +
      "The app renders and reads its OWN buffer, so this needs no compositor permission and works headlessly and over ssh — unlike every desktop screenshot route on Wayland, which an unattended caller cannot use. " +
      "The image is up to date with the command before it, so a set-param followed by a screenshot shows the new value.",
    inputSchema: {
      path: z.string().describe("Absolute path for the output .png file."),
      pid: pidArg,
    },
  },
  async ({ path, pid }) => one([`--screenshot ${quote(path)}`], pid),
);

// ---------------------------------------------------------------------------
// Driving the GUI
// ---------------------------------------------------------------------------

const coordNote =
  "Coordinates are in logical DIPs measured from the top-left of the WINDOW — the same space a screenshot is in at scale 1, so you can read them straight off the PNG. " +
  "The plugin's own editor starts `editorOriginY` pixels down (gmpi_info), because the menu bar is above it.";

server.registerTool(
  "gmpi_drag",
  {
    description:
      "Press, move and release — the way a user drags a knob or slider. " +
      "The whole gesture runs in one visit to the app's main thread, so it costs one round-trip rather than one per step. " +
      coordNote,
    inputSchema: {
      fromX: z.number(), fromY: z.number(),
      toX: z.number(), toY: z.number(),
      steps: z.number().int().min(1).max(1000).optional()
        .describe("Intermediate move events (default 10). More steps better approximates a slow human drag."),
      pid: pidArg,
    },
  },
  async ({ fromX, fromY, toX, toY, steps, pid }) =>
    one([`--drag ${fromX},${fromY} ${toX},${toY}${steps ? ` --steps ${steps}` : ""}`], pid),
);

server.registerTool(
  "gmpi_pointer",
  {
    description:
      "Send a single pointer event. Use gmpi_drag for an ordinary drag; these are for gestures it cannot express — a press, a look at the hover state, then a release somewhere else. " +
      "Events enter at the same input client a real mouse does, so capture and hover behave normally. " +
      coordNote,
    inputSchema: {
      action: z.enum(["down", "move", "up", "hover"])
        .describe("'hover' is a move with no button held; 'move' is a move mid-drag."),
      x: z.number(), y: z.number(),
      pid: pidArg,
    },
  },
  async ({ action, x, y, pid }) => {
    const verb = action === "hover" ? "--hover" : `--pointer-${action}`;
    return one([`${verb} ${x},${y}`], pid);
  },
);

// ---------------------------------------------------------------------------
// MIDI
// ---------------------------------------------------------------------------

server.registerTool(
  "gmpi_note",
  {
    description:
      "Play or release a MIDI note on the running instance — you will hear it if audio is running. " +
      "Injected at the same point a real MIDI cable enters, so it goes through the plugin's normal MIDI path. " +
      "For a note you can verify rather than hear, use gmpi_render_audio, which plays a note offline and reports the level it produced.",
    inputSchema: {
      action: z.enum(["on", "off"]),
      note: z.number().int().min(0).max(127).describe("MIDI note number; 60 is middle C."),
      velocity: z.number().int().min(0).max(127).optional().describe("Default 100 for note-on."),
      channel: z.number().int().min(1).max(16).optional().describe("1-16 as a musician counts them. Default 1."),
      pid: pidArg,
    },
  },
  async ({ action, note, velocity, channel, pid }) => {
    const ch = channel ?? 1;
    const vel = velocity ?? (action === "on" ? 100 : 0);
    return one([`--note-${action} ${ch} ${note} ${vel}`], pid);
  },
);

server.registerTool(
  "gmpi_all_notes_off",
  {
    description:
      "Send All Notes Off (CC 123) on every channel. The fix for a note left hanging — which is easy to do when injecting note-ons from a script.",
    inputSchema: { pid: pidArg },
  },
  async ({ pid }) => one(["--all-notes-off"], pid),
);

server.registerTool(
  "gmpi_cc",
  {
    description: "Send a MIDI control change to the running instance.",
    inputSchema: {
      controller: z.number().int().min(0).max(127).describe("Controller number, e.g. 1 for mod wheel."),
      value: z.number().int().min(0).max(127),
      channel: z.number().int().min(1).max(16).optional().describe("1-16. Default 1."),
      pid: pidArg,
    },
  },
  async ({ controller, value, channel, pid }) =>
    one([`--cc ${channel ?? 1} ${controller} ${value}`], pid),
);

server.registerTool(
  "gmpi_midi_raw",
  {
    description:
      "Send arbitrary MIDI bytes as hex, for anything the named tools do not cover (pitch bend, aftertouch, program change, sysex). Example: '90 3c 64' is note-on, middle C, velocity 100.",
    inputSchema: {
      bytes: z.string().describe("Space-separated hex bytes, e.g. 'e0 00 60' for pitch bend."),
      pid: pidArg,
    },
  },
  async ({ bytes, pid }) => one([`--midi ${bytes}`], pid),
);

// ---------------------------------------------------------------------------
// Hearing it
// ---------------------------------------------------------------------------

server.registerTool(
  "gmpi_render_audio",
  {
    description:
      "Render the plugin to a WAV file offline (faster than realtime) and report what it produced — peak, rms, clipped sample count, and a `silent` flag. " +
      "THE RESULT ANSWERS 'did it make the right sound' WITHOUT READING THE FILE, which is usually all you need. " +
      "Runs on its own processor instance primed with the CURRENT parameter values, so it neither disturbs nor is disturbed by whatever the app is playing — and it works even when no audio device is open. " +
      "Because it starts from the plugin's initial state, it is deterministic and repeatable; it is not a recording of what you are hearing right now. " +
      "Pass note to play something: without it a synth renders silence, which is a correct result and a useful control case.",
    inputSchema: {
      path: z.string().describe("Absolute path for the output .wav file."),
      seconds: z.number().min(0.01).max(240).optional().describe("Duration, default 2."),
      note: z.number().int().min(0).max(127).optional()
        .describe("MIDI note to play, held from t=0. Omit to render whatever the plugin does with no input."),
      velocity: z.number().int().min(1).max(127).optional().describe("Default 100."),
      channel: z.number().int().min(1).max(16).optional().describe("1-16. Default 1."),
      hold: z.number().min(0).optional()
        .describe("Seconds before note-off (default: half of `seconds`, leaving room for the release tail). Set >= seconds for a note that never ends."),
      rate: z.number().int().min(8000).max(384000).optional().describe("Sample rate, default 48000."),
      format: z.enum(["int16", "float32"]).optional()
        .describe("Default int16, which Python's `wave` module and sox can both read. float32 keeps headroom above 0 dBFS."),
      pid: pidArg,
    },
  },
  async ({ path, seconds, note, velocity, channel, hold, rate, format, pid }) => {
    const args = [`--render-audio ${quote(path)}`];
    if (seconds !== undefined) args.push(`--seconds ${seconds}`);
    if (note !== undefined) args.push(`--note ${note}`);
    if (velocity !== undefined) args.push(`--velocity ${velocity}`);
    if (channel !== undefined) args.push(`--channel ${channel}`);
    if (hold !== undefined) args.push(`--hold ${hold}`);
    if (rate !== undefined) args.push(`--rate ${rate}`);
    if (format !== undefined) args.push(`--format ${format}`);
    return one([args.join(" ")], pid);
  },
);

// ---------------------------------------------------------------------------
// Escape hatch
// ---------------------------------------------------------------------------

server.registerTool(
  "gmpi_script",
  {
    description:
      "Run several verbs in one round-trip, returning an array of result lines. " +
      "A batching convenience, not a requirement — every tool above works standalone, because the app holds the state. " +
      "Reach for this when you already know the whole sequence (e.g. set three parameters, then screenshot) and would otherwise make four calls. " +
      "Verbs are the app's own: --info, --list-params, --get-param, --set-param, --screenshot, --pointer-down/move/up, --hover, --drag, --note-on/off, --cc, --all-notes-off, --midi, --render-audio.",
    inputSchema: {
      commands: z.array(z.string()).min(1)
        .describe("One verb line per element, e.g. ['--set-param 7 30', '--screenshot /tmp/a.png']."),
      pid: pidArg,
    },
  },
  async ({ commands, pid }) => {
    try {
      const { lines, app } = await run(commands, pid);
      return ok({ ok: true, pid: app.pid, results: lines },
                lines.some((l: any) => l?.ok === false));
    } catch (e: any) {
      return ok({ ok: false, error: e?.message ?? String(e) }, true);
    }
  },
);

// ---------------------------------------------------------------------------

const transport = new StdioServerTransport();
await server.connect(transport);
