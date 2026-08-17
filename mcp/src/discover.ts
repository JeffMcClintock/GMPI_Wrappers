// Finding the running standalone apps.
//
// A directory listing IS the discovery mechanism - the app publishes nothing
// else, so there is no registry, config file or port to keep in sync. Same
// design as SynthEdit's live transport, and the leaf name carries the pid for
// the same reason: it is the only identifier available before connecting.

import { readdirSync, existsSync } from "node:fs";
import { join } from "node:path";

const PREFIX = "gmpi-standalone.";

export interface RunningApp {
  /** OS process id of the app. */
  pid: number;
  /** Full path of the unix socket to connect to. */
  path: string;
}

/**
 * The directories a standalone may publish its command channel in, in the same
 * order the app itself tries them (see mcp/IpcServer.h: chooseSocketPath).
 *
 * XDG_RUNTIME_DIR first because that is where a desktop session belongs: it is
 * per-user, mode 0700, on tmpfs, and cleared at logout. The /tmp fallback
 * exists for a bare ssh session or a container, where the pam module that
 * creates XDG_RUNTIME_DIR never ran.
 */
export function channelDirs(): string[] {
  const override = process.env.GMPI_STANDALONE_IPC_DIR;
  if (override) return [override];

  const dirs: string[] = [];
  const uid = process.getuid?.();

  if (process.env.XDG_RUNTIME_DIR)
    dirs.push(join(process.env.XDG_RUNTIME_DIR, "gmpi-standalone"));

  // The SAME path XDG_RUNTIME_DIR conventionally names, derived rather than
  // read. Not redundant: an MCP host does not hand its servers the user's
  // whole environment - the SDK spawns them with a small allow-list that
  // XDG_RUNTIME_DIR is not on - so the variable is typically ABSENT here even
  // though the app that published the socket had it. Without this, discovery
  // finds nothing under a real MCP host while working perfectly from a shell,
  // which is a maddening way to fail.
  if (uid !== undefined) dirs.push(`/run/user/${uid}/gmpi-standalone`);

  if (uid !== undefined) dirs.push(`/tmp/gmpi-standalone.${uid}`);

  return [...new Set(dirs)];
}

/**
 * True if a process with this pid still exists.
 *
 * A unix socket file does NOT evaporate with its process, so a crashed or
 * force-quit app leaves a corpse behind. Without pruning, "2 apps are running -
 * pass pid to choose one" fires against ghosts, and after a crash that is the
 * normal case rather than the exception. (The app unlinks its own socket on a
 * clean exit, and unlinks a stale one before binding, so this only matters
 * between a crash and the next launch.)
 *
 * /proc is the cheap answer on Linux. kill(pid, 0) is the portable one, and it
 * is only consulted as a fallback because it cannot distinguish "not running"
 * from "running as another user" without inspecting errno.
 */
function pidAlive(pid: number): boolean {
  if (existsSync(`/proc/${pid}`)) return true;
  if (existsSync("/proc")) return false;   // procfs exists and said no: trust it

  try {
    process.kill(pid, 0);
    return true;
  } catch (e: any) {
    return e?.code === "EPERM";
  }
}

/** Every running standalone that has a command channel open, lowest pid first. */
export function discoverApps(): RunningApp[] {
  const found = new Map<number, RunningApp>();

  for (const dir of channelDirs()) {
    let names: string[];
    try {
      names = readdirSync(dir);
    } catch {
      continue;   // absent directory just means nothing published here
    }

    for (const name of names) {
      if (!name.startsWith(PREFIX)) continue;

      const pid = Number(name.slice(PREFIX.length));
      if (!Number.isInteger(pid) || pid <= 0) continue;
      if (found.has(pid)) continue;
      if (!pidAlive(pid)) continue;

      found.set(pid, { pid, path: join(dir, name) });
    }
  }

  return [...found.values()].sort((a, b) => a.pid - b.pid);
}
