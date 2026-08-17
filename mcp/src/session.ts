// The socket connection to one running standalone.
//
// Newline-framed command lines out, one JSON object per line back. Each batch
// is terminated with `--ping <uuid>` and read until that token echoes, rather
// than counting result lines: a batch can carry several verbs and so produce
// several lines, and a naive one-line-per-request pairing desyncs the moment
// anything emits an unexpected number of them.
//
// UNLIKE SynthEdit's headless session, the connection here holds NO state. The
// app owns the document, the parameters and the GUI, and it stays running
// whether or not anyone is connected - so a dropped socket costs nothing and
// reconnecting is free. That is why there is no `sessionLost` concept: losing
// the connection is not losing the work. Only the app exiting is real loss,
// and that reports as "no running app".

import net from "node:net";
import { randomUUID } from "node:crypto";
import { discoverApps, type RunningApp } from "./discover.js";

/** How long one batch may take before we give up on it. */
const CALL_TIMEOUT_MS = 5 * 60_000;

export interface CallResult {
  /** One parsed object per result line, in order. */
  lines: any[];
  /** The app this went to. */
  app: RunningApp;
}

export class NoAppError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "NoAppError";
  }
}

/**
 * Picks which app to talk to.
 *
 * With exactly one running, no choice is needed and none is asked for - the
 * overwhelmingly common case is a developer with one plugin open. Ambiguity is
 * only reported when it is real.
 */
export function resolveApp(pid?: number): RunningApp {
  const apps = discoverApps();

  if (apps.length === 0)
    throw new NoAppError(
      "No running GMPI standalone found. Launch one (e.g. SawDemo_STANDALONE) and try again; " +
      "it prints 'command channel: <path>' on startup when the channel is open.");

  if (pid !== undefined) {
    const match = apps.find(a => a.pid === pid);
    if (!match)
      throw new NoAppError(
        `No running standalone with pid ${pid}. Running: ${apps.map(a => a.pid).join(", ")}`);
    return match;
  }

  if (apps.length > 1)
    throw new NoAppError(
      `${apps.length} standalones are running (pids ${apps.map(a => a.pid).join(", ")}). ` +
      "Pass pid to choose one.");

  return apps[0];
}

/**
 * Runs one batch of verbs against one app and returns the parsed result lines.
 *
 * A connection per call, deliberately. The app holds the state, so a persistent
 * socket would buy nothing but a reconnect path to get wrong - and an idle
 * client holding a connection open is exactly what made SynthEdit's editor look
 * wedged (see the kMaxClients note in IpcServer.h).
 */
export function call(app: RunningApp, commands: string[]): Promise<CallResult> {
  return new Promise((resolve, reject) => {
    const token = randomUUID();
    const socket = net.createConnection(app.path);

    let inbox = "";
    const lines: any[] = [];
    let settled = false;

    const finish = (fn: () => void) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      socket.destroy();
      fn();
    };

    const timer = setTimeout(() => {
      finish(() => reject(new Error(
        `The app did not answer within ${CALL_TIMEOUT_MS / 1000}s. Its main thread may be blocked.`)));
    }, CALL_TIMEOUT_MS);

    socket.on("error", (err: any) => {
      // ECONNREFUSED against a socket file that exists means a corpse: the app
      // died without unlinking. Say so in the terms the user can act on.
      const hint = err?.code === "ECONNREFUSED"
        ? ` (pid ${app.pid} left a stale socket behind; it is no longer running)`
        : "";
      finish(() => reject(new Error(`${err?.message ?? String(err)}${hint}`)));
    });

    socket.on("close", () => {
      // Closing before the sentinel means the app went away mid-batch - almost
      // always a crash, which is worth naming rather than reporting as a
      // generic parse failure.
      finish(() => reject(new Error(
        `The app closed the connection before finishing (pid ${app.pid} may have crashed). ` +
        `Received ${lines.length} result line(s).`)));
    });

    socket.on("data", chunk => {
      inbox += chunk.toString("utf8");

      let nl: number;
      while ((nl = inbox.indexOf("\n")) !== -1) {
        const line = inbox.slice(0, nl).trim();
        inbox = inbox.slice(nl + 1);
        if (!line) continue;

        let parsed: any;
        try {
          parsed = JSON.parse(line);
        } catch {
          // Not JSON at all. Keep it rather than dropping it: it is the only
          // evidence of whatever went wrong.
          lines.push({ ok: false, error: "unparsable response", raw: line });
          continue;
        }

        if (parsed?.cmd === "ping" && parsed?.token === token) {
          finish(() => resolve({ lines, app }));
          return;
        }

        lines.push(parsed);
      }
    });

    socket.on("connect", () => {
      const batch = [...commands, `--ping ${token}`].join("\n") + "\n";
      socket.write(batch);
    });
  });
}

/** Convenience: resolve the app, run the verbs, return the parsed lines. */
export async function run(commands: string[], pid?: number): Promise<CallResult> {
  return call(resolveApp(pid), commands);
}
