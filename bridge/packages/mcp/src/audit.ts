import { appendFileSync, mkdirSync, renameSync, statSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join } from "node:path";

export interface AuditEntry {
  tool: string;
  args?: Record<string, unknown>;
  ok: boolean;
  status?: string;
  ms: number;
}

const MAX_BYTES = 1024 * 1024;

/** Append-only JSON-lines record of every tool call (never the file contents). Disabled with path = null. */
export class Audit {
  readonly path: string | null;
  constructor(path: string | null = join(homedir(), ".config", "nintendo-dev", "audit.log")) {
    this.path = path;
  }

  record(e: AuditEntry): void {
    if (!this.path) return;
    try {
      mkdirSync(dirname(this.path), { recursive: true });
      try {
        if (statSync(this.path).size > MAX_BYTES) renameSync(this.path, `${this.path}.1`);
      } catch {
        /* no file yet */
      }
      appendFileSync(this.path, `${JSON.stringify({ ts: new Date().toISOString(), ...e })}\n`);
    } catch {
      /* auditing must never break a tool call */
    }
  }
}
