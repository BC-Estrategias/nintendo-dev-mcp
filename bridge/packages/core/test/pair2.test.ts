// Pairing by number comparison (spec §4.8): the vectors from the independent Python reference, then the whole protocol against
// the C host agent, including the ways a man in the middle or a careless client could try to get a pairing the person never
// compared.
import { strict as assert } from "node:assert";
import { readFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { type ChildProcess } from "node:child_process";
import {
  Command, NdpClient, NdpRemoteError, PairingRefusedError, Tag, encodeTlv, formatSas, pair2Commit, pair2Derive, pair2OkProof, parseTlv,
  randomNonce, randomSecret, x25519Public, x25519Shared,
} from "../src/index.ts";
import { SKIP, startAgent } from "./helpers.ts";

const V = JSON.parse(readFileSync(fileURLToPath(new URL("../../../../docs/protocol/test-vectors/vectors.json", import.meta.url)), "utf8"));
const fromHex = (h: string) => new Uint8Array(Buffer.from(h, "hex"));
const hex = (b: Uint8Array) => Buffer.from(b).toString("hex");
const suite = SKIP ? describe.skip : describe;
const code = (p: Promise<unknown>) => p.then(() => "ok", (e) => (e instanceof NdpRemoteError ? e.statusName : `${(e as Error).name}`));

test("X25519 (node:crypto) and the pairing derivations agree with the Python reference and the RFC 7748 vectors", () => {
  for (const e of V.pair2.x25519) assert.equal(hex(x25519Shared(fromHex(e.scalar_hex), fromHex(e.point_hex))!), e.out_hex);
  for (const lo of V.pair2.x25519_low_order) assert.equal(x25519Shared(fromHex(V.pair2.x25519[0].scalar_hex), fromHex(lo)), null, "low-order points are refused");
  for (const p of V.pair2.pair2) {
    const sb = fromHex(p.secret_b_hex), sc = fromHex(p.secret_c_hex), label = new TextEncoder().encode(p.label);
    const pubB = x25519Public(sb), pubC = x25519Public(sc);
    assert.equal(hex(pubB), p.pub_b_hex);
    assert.equal(hex(pubC), p.pub_c_hex);
    const sh = x25519Shared(sb, pubC)!;
    assert.equal(hex(sh), p.shared_hex);
    assert.equal(hex(x25519Shared(sc, pubB)!), p.shared_hex);
    assert.equal(hex(pair2Commit(pubB, fromHex(p.nonce_b_hex))), p.commit_hex);
    const k = pair2Derive(sh, pubB, pubC, fromHex(p.nonce_b_hex), fromHex(p.nonce_c_hex), label);
    assert.equal(k.sas, p.sas);
    assert.equal(hex(k.psk), p.psk_hex);
    assert.equal(hex(k.keyId), p.key_id_hex);
    assert.equal(hex(pair2OkProof(k.psk, pubB, pubC)), p.ok_proof_hex);
    assert.equal(formatSas(k.sas).replace(" ", ""), p.sas_text);
  }
});

suite("pairing by number comparison against the host agent", () => {
  const dirs: string[] = [];
  const procs: ChildProcess[] = [];
  after(() => { procs.forEach((p) => p.kill()); dirs.forEach((d) => rmSync(d, { recursive: true, force: true })); });
  async function agent(extra: string[]) {
    const dir = mkdtempSync(join(tmpdir(), "ndp-p2-"));
    dirs.push(dir);
    const a = await startAgent(["--root", dir, "--mode", "DEVELOPMENT", "--auth", "required", "--keys-file", join(dir, "keys.bin"), "--pair-open", ...extra]);
    procs.push(a.proc);
    return a;
  }
  const connect = async (port: number) => { const c = await NdpClient.connect({ host: "127.0.0.1", port }); const info = await c.hello(); return { c, info }; };
  const numberOf = (out: string) => /number=(\d{6})/.exec(out)?.[1];

  test("the number on the console equals the number the client computed; the key then authenticates; the window is single-use", async () => {
    const a = await agent(["--pair-approve", "auto"]);
    const { c, info } = await connect(a.port);
    assert.equal(info.pairingOpen, true);
    let shown = "";
    const { psk } = await c.pairByNumber("Test PC", (sas) => (shown = sas));
    assert.equal(shown.replace(" ", ""), numberOf(a.stdout()), "both screens show the same number");
    assert.match(a.stdout(), /PAIR2 label="Test PC"/);
    assert.ok(!/number/.test(a.log()), "the number is never written to the agent's log");
    c.close();
    const c2 = await NdpClient.connect({ host: "127.0.0.1", port: a.port });
    const info2 = await c2.hello();
    assert.equal(info2.pairedKeys, 1);
    assert.equal(info2.pairingOpen, false, "one pairing closes the window");
    await c2.authenticate(psk);
    assert.equal(await c2.ping() > 0, true);
    c2.close();
    const c3 = await NdpClient.connect({ host: "127.0.0.1", port: a.port });
    await c3.hello();
    assert.equal(await code(c3.pairByNumber("Again", () => undefined)), "UNAUTHORIZED", "no second pairing without pressing Y again");
    c3.close();
  });

  test("when the person says no, the client gets a refusal and no key", async () => {
    const a = await agent(["--pair-approve", "deny"]);
    const { c } = await connect(a.port);
    await assert.rejects(c.pairByNumber("Nope", () => undefined), (e: Error) => e instanceof PairingRefusedError && e.reason === "denied");
    c.close();
    const c2 = (await connect(a.port));
    assert.equal(c2.info.pairedKeys, 0);
    assert.equal(c2.info.pairingOpen, true, "one refusal does not close the window");
    c2.c.close();
  });

  test("nobody at the console: the request expires and is refused; and five refusals close the window", async () => {
    const a = await agent(["--pair-approve", "never", "--pair-prompt-ms", "400"]);
    const { c } = await connect(a.port);
    const t0 = Date.now();
    await assert.rejects(c.pairByNumber("Late", () => undefined, { pollMs: 100 }), (e: Error) => e instanceof PairingRefusedError);
    assert.ok(Date.now() - t0 < 5000);
    c.close();
    const b = await agent(["--pair-approve", "deny"]);
    for (let i = 0; i < 5; i++) {
      const { c: cc } = await connect(b.port);
      await assert.rejects(cc.pairByNumber("x", () => undefined), PairingRefusedError);
      cc.close();
    }
    const { c: last, info } = await connect(b.port);
    assert.equal(info.pairingOpen, false, "five refused requests close the window");
    assert.equal(await code(last.pairByNumber("x", () => undefined)), "UNAUTHORIZED");
    last.close();
  });

  test("a client that changes its key after seeing the console's (or lies in the commitment) never gets a number on the console", async () => {
    const a = await agent(["--pair-approve", "auto"]);
    const { c } = await connect(a.port);
    const secret = randomSecret(), pubB = x25519Public(secret), nonceB = randomNonce();
    const label = new TextEncoder().encode("Cheat");
    const begin = parseTlv((await c.request(Command.PAIR_BEGIN, encodeTlv([[Tag.LABEL, label], [Tag.PAIR_COMMIT, pair2Commit(pubB, nonceB)]]))).payload);
    assert.equal(begin.first(Tag.PAIR_PUB)!.length, 32);
    // reveal a DIFFERENT key than the one committed
    const other = x25519Public(randomSecret());
    assert.equal(await code(c.request(Command.PAIR_REVEAL, encodeTlv([[Tag.PAIR_PUB, other], [Tag.PAIR_NONCE, nonceB]]))), "UNAUTHORIZED");
    await new Promise((r) => setTimeout(r, 300));
    assert.equal(numberOf(a.stdout()), undefined, "no number was ever shown for it");
    c.close();
  });

  test("low-order public keys, reveal without begin, poll without a request, and out-of-order messages are refused", async () => {
    const a = await agent(["--pair-approve", "never"]);
    const { c } = await connect(a.port);
    const label = new TextEncoder().encode("Odd");
    assert.equal(await code(c.request(Command.PAIR_POLL, new Uint8Array(0))), "BAD_REQUEST");
    assert.equal(await code(c.request(Command.PAIR_REVEAL, encodeTlv([[Tag.PAIR_PUB, new Uint8Array(32).fill(1)], [Tag.PAIR_NONCE, new Uint8Array(16)]]))), "BAD_REQUEST");
    for (const lo of V.pair2.x25519_low_order as string[]) {
      const nonce = randomNonce(), pub = x25519Public(randomSecret());
      // commit to the low-order point itself so the commitment check passes and only the key check can refuse it
      await c.request(Command.PAIR_BEGIN, encodeTlv([[Tag.LABEL, label], [Tag.PAIR_COMMIT, pair2Commit(fromHex(lo), nonce)]]));
      assert.equal(await code(c.request(Command.PAIR_REVEAL, encodeTlv([[Tag.PAIR_PUB, fromHex(lo)], [Tag.PAIR_NONCE, nonce]]))), "BAD_REQUEST", `low-order ${lo.slice(0, 8)}`);
      void pub;
    }
    assert.equal(await code(c.request(Command.PAIR_BEGIN, encodeTlv([[Tag.LABEL, label]]))), "BAD_REQUEST", "commit required");
    assert.equal(await code(c.request(Command.PAIR_BEGIN, encodeTlv([[Tag.LABEL, new Uint8Array(16)], [Tag.PAIR_COMMIT, new Uint8Array(32)]]))), "BAD_REQUEST", "label too long");
    c.close();
  });

  test("a second connection cannot take over or poll another connection's pending request", async () => {
    const a = await agent(["--pair-approve", "never", "--pair-prompt-ms", "20000"]);
    const one = await connect(a.port);
    const secret = randomSecret(), pubB = x25519Public(secret), nonceB = randomNonce();
    const label = new TextEncoder().encode("First");
    const begin = parseTlv((await one.c.request(Command.PAIR_BEGIN, encodeTlv([[Tag.LABEL, label], [Tag.PAIR_COMMIT, pair2Commit(pubB, nonceB)]]))).payload);
    await one.c.request(Command.PAIR_REVEAL, encodeTlv([[Tag.PAIR_PUB, pubB], [Tag.PAIR_NONCE, nonceB]]));
    void begin;
    // the number is on the console now; a second (web) connection is a different owner. Here a second raw client replaces the first
    // connection (raw clients replace each other), which cancels its request: nothing pending is left to hijack
    const two = await connect(a.port);
    assert.equal(await code(two.c.request(Command.PAIR_POLL, new Uint8Array(0))), "BAD_REQUEST", "nothing to poll on a new connection");
    one.c.close();
    two.c.close();
  });
});
