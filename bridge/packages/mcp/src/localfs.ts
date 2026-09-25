import { existsSync, realpathSync } from "node:fs";
import { dirname, isAbsolute, resolve, sep } from "node:path";

/**
 * The LLM may only read/write local files inside these roots (default: the server's working directory).
 * This stops a prompt-injected request from uploading ~/.ssh to the console or overwriting local files.
 */
export class LocalRoots {
  readonly roots: string[];
  constructor(roots: string[]) {
    this.roots = roots.map((r) => realpathSync(resolve(r)));
  }

  /** Resolves `p` (relative to the first root) and checks it stays inside a root, following symlinks. */
  resolve(p: string): string {
    const abs = isAbsolute(p) ? resolve(p) : resolve(this.roots[0] as string, p);
    // realpath the deepest existing ancestor so symlinked directories cannot lead outside a root
    let probe = abs;
    while (!existsSync(probe)) {
      const up = dirname(probe);
      if (up === probe) break;
      probe = up;
    }
    const realProbe = realpathSync(probe);
    const real = probe === abs ? realProbe : resolve(realProbe, abs.slice(probe.length + 1));
    const ok = this.roots.some((r) => real === r || real.startsWith(r.endsWith(sep) ? r : r + sep));
    if (!ok) throw new LocalPathError(`local path ${p} is outside the allowed local folders (${this.roots.join(", ")})`);
    return real;
  }
}

export class LocalPathError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "LocalPathError";
  }
}
