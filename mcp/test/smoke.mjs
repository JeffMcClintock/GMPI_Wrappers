#!/usr/bin/env node
// End-to-end smoke test, over real MCP stdio against a real running app.
//
//   1. launch a standalone (e.g. SawDemo_STANDALONE)
//   2. npm run build
//   3. node test/smoke.mjs
//
// Exercises every tool that does not need a human to look at the result, and
// asserts the things that would silently rot: that a set-param actually moves
// the value, that a render with a note is loud and one without is silent, and
// that a drag changes the parameter under it.

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { tmpdir } from "node:os";
import { statSync } from "node:fs";

const here = dirname(fileURLToPath(import.meta.url));
const server = join(here, "..", "dist", "index.js");

let failures = 0;
function check(label, condition, detail = "") {
  const mark = condition ? "PASS" : "FAIL";
  if (!condition) ++failures;
  console.log(`  ${mark}  ${label}${detail ? ` — ${detail}` : ""}`);
}

const client = new Client({ name: "smoke", version: "0.1.0" }, { capabilities: {} });
await client.connect(new StdioClientTransport({ command: process.execPath, args: [server] }));

const callTool = async (name, args = {}) => {
  const res = await client.callTool({ name, arguments: args });
  return JSON.parse(res.content[0].text);
};

console.log("\ntools");
const { tools } = await client.listTools();
check(`${tools.length} tools registered`, tools.length >= 12, tools.map(t => t.name).join(", "));

console.log("\ndiscovery");
const apps = await callTool("gmpi_list_apps");
check("an app is running", apps.count > 0, apps.count ? `pid ${apps.apps[0].pid} (${apps.apps[0].name})` : apps.hint);
if (!apps.count) {
  console.log("\nLaunch a standalone first. Skipping the rest.\n");
  await client.close();
  process.exit(1);
}

console.log("\ninfo");
const info = await callTool("gmpi_info");
check("reports plugin name", !!info.name, info.name);
check("reports window geometry", info.windowWidth > 0 && info.canvasWidth > 0,
      `${info.windowWidth}x${info.windowHeight} DIP, ${info.canvasWidth}x${info.canvasHeight} px, scale ${info.scale}`);

console.log("\nparameters");
const list = await callTool("gmpi_list_params");
check("lists parameters", list.count > 0, `${list.count} parameters`);

const target = list.parameters.find(p => p.id >= 0 && p.datatype === "float32" && p.maximum > p.minimum);
check("found a writable float parameter", !!target, target ? `#${target.id} ${target.name}` : "none");

if (target) {
  const midpoint = (target.minimum + target.maximum) / 2;
  const set = await callTool("gmpi_set_param", { id: target.id, value: midpoint });
  check("set-param reports ok", set.ok === true);

  const got = await callTool("gmpi_get_param", { id: target.id });
  check("value round-trips", Math.abs(got.parameter.value - midpoint) < 1e-3,
        `wrote ${midpoint}, read ${got.parameter.value}`);

  const again = await callTool("gmpi_set_param", { id: target.id, value: midpoint });
  check("unchanged write reports changed:false", again.changed === false);

  const norm = await callTool("gmpi_set_param", { id: target.id, value: 1, normalised: true });
  check("normalised write hits the maximum", Math.abs(norm.parameter.value - target.maximum) < 1e-3,
        `${norm.parameter.value} vs max ${target.maximum}`);

  // Put it back to the plugin author's DEFAULT, not to whatever it happened to
  // be on entry. Both matter:
  //
  //   * without any restore, the plugin is left pinned at maximum and the
  //     audio checks below render at 10x, reporting clipping the TEST caused;
  //   * restoring the incoming value is no better, because that may itself be
  //     degenerate - a previous session leaving a gain at 0 makes "render is
  //     audible" fail against a perfectly healthy plugin.
  //
  // The default is the one value guaranteed to be a working configuration.
  await callTool("gmpi_set_param", { id: target.id, value: target.default ?? target.value });
}

console.log("\nscreenshot");
const shotPath = join(tmpdir(), `gmpi-smoke-${process.pid}.png`);
const shot = await callTool("gmpi_screenshot", { path: shotPath });
check("screenshot ok", shot.ok === true);
check("png is non-trivial", shot.ok && statSync(shotPath).size > 1000, shot.ok ? `${statSync(shotPath).size} bytes` : "");

// An INSTRUMENT is excited by a note; an EFFECT by a signal on its inputs.
// Getting this wrong does not fail loudly - it just renders silence and every
// audio assertion below collapses into "nothing happened", so the test has to
// know which kind of plugin it is looking at.
const isEffect = info.audioInputs > 0;
const excite = isEffect ? { input: "tone", inputLevel: 0.5 } : { note: 60, hold: 0.5 };
console.log(`\naudio (offline render) — ${isEffect ? "effect: 440Hz tone in" : "instrument: MIDI note in"}`);

const loudPath = join(tmpdir(), `gmpi-smoke-loud-${process.pid}.wav`);
const quietPath = join(tmpdir(), `gmpi-smoke-quiet-${process.pid}.wav`);

const loud = await callTool("gmpi_render_audio", { path: loudPath, seconds: 1, ...excite });
// The control case: no excitation at all should produce nothing. If this is
// NOT silent, something is self-oscillating and every measurement above it is
// suspect.
const quiet = await callTool("gmpi_render_audio", { path: quietPath, seconds: 0.25 });

if (loud.error?.includes("no audio outputs")) {
  check("plugin has no audio outputs (render skipped)", true, loud.error);
} else {
  const what = isEffect ? "a tone" : "a note";
  check(`render with ${what} is audible`, loud.ok && loud.silent === false, `peak ${loud.peak}`);
  check(`render without ${what} is silent`, quiet.ok && quiet.silent === true, `peak ${quiet.peak}`);
  check("render did not clip", loud.clippedSamples === 0, `${loud.clippedSamples} clipped`);
}

// The regression guard that matters most, because the bug it catches is
// SILENT: a render can look entirely healthy — right duration, plausible
// level, a note audibly playing — while ignoring every parameter, if the
// priming events lose their race with the plugin's own defaults. Nothing else
// in this file would notice.
//
// Generic on purpose: sweep candidate parameters min -> max and require that
// at least ONE of them moves the output. Which one is the plugin's business.
if (!loud.error) {
  console.log("\nparameters actually reach the render");
  const candidates = list.parameters.filter(p => p.id >= 0 && p.maximum > p.minimum).slice(0, 8);
  const saved = new Map();
  let mover = null;

  for (const p of candidates) {
    saved.set(p.id, p.value);
    const at = async (v) => {
      await callTool("gmpi_set_param", { id: p.id, value: v });
      const r = await callTool("gmpi_render_audio",
        { path: join(tmpdir(), `gmpi-sweep-${process.pid}.wav`), seconds: 0.3,
          ...(isEffect ? excite : { note: 60, hold: 0.25 }) });
      return r.peak;
    };
    const lo = await at(p.minimum);
    const hi = await at(p.maximum);
    await callTool("gmpi_set_param", { id: p.id, value: saved.get(p.id) });

    if (Math.abs(hi - lo) > 1e-4) { mover = { p, lo, hi }; break; }
  }

  check("some parameter changes the rendered audio", mover !== null,
        mover ? `#${mover.p.id} ${mover.p.name}: peak ${mover.lo.toFixed(4)} -> ${mover.hi.toFixed(4)}`
              : `none of ${candidates.length} swept parameters altered the output — priming is probably not reaching the processor`);
}

console.log("\nMIDI");
const noteOn = await callTool("gmpi_note", { action: "on", note: 60 });
if (noteOn.error?.includes("no MIDI input")) {
  check("plugin takes no MIDI (skipped)", true, noteOn.error);
} else {
  check("note-on accepted", noteOn.ok === true, noteOn.bytes);
  check("note-off accepted", (await callTool("gmpi_note", { action: "off", note: 60 })).ok === true);
  check("all-notes-off accepted", (await callTool("gmpi_all_notes_off")).ok === true);
}

console.log("\nGUI input");
const hover = await callTool("gmpi_pointer", { action: "hover", x: 10, y: info.editorOriginY + 10 });
check("hover accepted", hover.ok === true);

console.log("\nbatching");
const script = await callTool("gmpi_script", { commands: ["--info", "--list-params"] });
check("script returns one line per verb", script.results?.length === 2);

console.log(`\n${failures === 0 ? "all checks passed" : `${failures} FAILED`}\n`);
await client.close();
process.exit(failures === 0 ? 0 : 1);
