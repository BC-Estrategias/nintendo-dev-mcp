// The page's pure logic (web/js/util.js) and its dictionaries (web/js/i18n.js), run in Node. The DOM parts are checked
// in a real browser against the host agent (see scripts/check.sh notes); here we guard what can silently rot.
import { strict as assert } from "node:assert";
import { readFileSync, readdirSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

const WEB = fileURLToPath(new URL("../../../../web/js/", import.meta.url));
for (const f of ["util.js", "i18n.js"]) await import(pathToFileURL(WEB + f).href);
const NDP = (globalThis as any).NDP;
const U = NDP.util;

test("util: paths follow the console's rules (component-wise, ASCII case folded)", () => {
  assert.ok(U.inside("/3ds/Foo", "/3DS"));
  assert.ok(U.inside("/3ds", "/3ds"));
  assert.ok(!U.inside("/3dsx", "/3ds"));
  assert.ok(!U.inside("/", "/3ds"));
  assert.ok(U.inside("/anything/at/all", "/"));
  assert.equal(U.join("/", "a"), "/a");
  assert.equal(U.join("/a/b", "c"), "/a/b/c");
  assert.equal(U.parent("/a/b"), "/a");
  assert.equal(U.parent("/a"), "/");
  assert.equal(U.parent("/"), "/");
  assert.equal(U.basename("/a/b.txt"), "b.txt");
  assert.equal(U.ext("Photo.PNG"), "png");
  assert.equal(U.ext(".gitignore"), "");
  assert.equal(U.ext("noext"), "");
  assert.deepEqual(U.crumbs("/a/b").map((c: any) => c.path), ["/", "/a", "/a/b"]);
  assert.deepEqual(U.crumbs("/").map((c: any) => c.path), ["/"]);
  assert.equal(U.trashRootOf("/3ds/x/.ndp-trash/a.1"), "/3ds/x/.ndp-trash");
  assert.equal(U.trashRootOf("/3ds/x/.ndp-trash"), "/3ds/x/.ndp-trash");
  assert.equal(U.trashRootOf("/3ds/x/.ndp-trashy/a"), null);
  assert.equal(U.trashRootOf("/3ds/x"), null);
});

test("util: sizes, rates and times are readable", () => {
  const cases: Array<[number, string]> = [[0, "0 B"], [1023, "1023 B"], [1024, "1.00 KiB"], [1536, "1.50 KiB"], [10 * 1024, "10.0 KiB"], [100 * 1024, "100 KiB"], [1 << 20, "1.00 MiB"], [5 * 2 ** 30, "5.00 GiB"]];
  for (const [n, s] of cases) assert.equal(U.formatSize(n), s);
  assert.equal(U.formatSize(undefined), "");
  assert.equal(U.formatRate(0), "");
  assert.equal(U.formatRate(1 << 20), "1.00 MiB/s");
  assert.equal(U.formatEta(5.2), "6 s");
  assert.equal(U.formatEta(125), "2 min 5 s");
  assert.equal(U.formatEta(7300), "2 h 1 min");
  assert.equal(U.formatEta(Infinity), "");
});

test("util: file kinds and binary detection", () => {
  assert.ok(U.isImage("a.PNG") && U.isImage("b.jpeg") && !U.isImage("c.txt"));
  assert.ok(U.isTextName("notes.md") && U.isTextName("Makefile") && U.isTextName("README") && !U.isTextName("game.3dsx"));
  assert.equal(U.imageMime("a.jpg"), "image/jpeg");
  assert.ok(U.looksBinary(new Uint8Array([65, 0, 66])));
  assert.ok(U.looksBinary(new Uint8Array(100).fill(3)));
  assert.ok(!U.looksBinary(new TextEncoder().encode("hello\nworld\t\r\n")));
  assert.ok(!U.looksBinary(new Uint8Array(0)));
});

test("util: unique names and name validation", () => {
  const taken = new Set(["a.txt", "a (1).txt", "noext", ".hidden"]);
  assert.equal(U.uniqueName("b.txt", taken), "b.txt");
  assert.equal(U.uniqueName("A.TXT", taken), "A (2).TXT");
  assert.equal(U.uniqueName("noext", taken), "noext (1)");
  assert.equal(U.uniqueName(".hidden", taken), ".hidden (1)");
  for (const [n, want] of [["", "name.empty"], [".", "name.empty"], ["..", "name.empty"], ["a/b", "name.chars"], ["a:b", "name.chars"], ["a\u0001", "name.chars"], ["bad.", "name.trailing"], ["bad ", "name.trailing"], ["x~1.txt", "name.tilde"], ["é".repeat(128), "name.long"], ["ok name.txt", null], ["é".repeat(127), null]] as Array<[string, string | null]>) {
    assert.equal(U.nameProblem(n), want, JSON.stringify(n));
  }
});

test("util: folders first, natural numeric order, size sort", () => {
  const e = (name: string, type = "file", size = 0) => ({ name, type, size });
  const list = [e("file10"), e("file2"), e("Zed", "dir"), e("alpha", "dir"), e("file1")];
  assert.deepEqual(U.sortEntries(list, "name", "asc").map((x: any) => x.name), ["alpha", "Zed", "file1", "file2", "file10"]);
  assert.deepEqual(U.sortEntries(list, "name", "desc").map((x: any) => x.name), ["Zed", "alpha", "file10", "file2", "file1"]);
  const bySize = [e("a", "file", 5), e("b", "file", 50), e("c", "dir", 0), e("d", "file", 1)];
  assert.deepEqual(U.sortEntries(bySize, "size", "desc").map((x: any) => x.name), ["c", "b", "a", "d"]);
});

test("util: what the page promises about a path matches the console's policy", () => {
  for (const p of ["/luma", "/luma/config.ini", "/Nintendo 3DS/x", "/boot.firm", "/gm9/x", "/private", "/3ds/nintendo-dev-agent/config/pairing.bin"]) assert.ok(U.writeProtected(p), p);
  for (const p of ["/luma/plugins", "/luma/plugins/a.3gx", "/luma/titles/0004/code.bin", "/3ds/app", "/", "/lumafoo"]) assert.ok(!U.writeProtected(p), p);
  const access = { readRoots: ["/3ds", "/DCIM"], writeRoots: ["/3ds/nintendo-dev-agent", "/luma"] };
  assert.equal(U.accessOf("/3ds/nintendo-dev-agent/x", access, "DEVELOPMENT"), "write");
  assert.equal(U.accessOf("/3ds/nintendo-dev-agent/x", access, "READ_ONLY"), "read");
  assert.equal(U.accessOf("/luma/config.ini", access, "DEVELOPMENT"), "none"); // protected, and not readable by the lists
  assert.equal(U.accessOf("/3ds/other", access, "DEVELOPMENT"), "read");
  assert.equal(U.accessOf("/dcim/a.jpg", access, "DEVELOPMENT"), "read");
  assert.equal(U.accessOf("/etc", access, "DEVELOPMENT"), "none");
  assert.equal(U.accessOf("/anything", null, "DEVELOPMENT"), "read");
});

// ------------------------------------------------------------------------------------------------ i18n

const placeholders = (s: string) => [...s.matchAll(/\{(\d+)\}/g)].map((m) => m[1]).sort();

test("i18n: both dictionaries have exactly the same keys and placeholders", () => {
  const { "pt-BR": pt, en } = NDP.i18n.DICT as { "pt-BR": any; en: any };
  assert.deepEqual(Object.keys(pt).sort(), Object.keys(en).sort());
  for (const k of Object.keys(pt)) {
    assert.equal(typeof pt[k], "string", k);
    assert.ok(pt[k].length > 0 && en[k].length > 0, `empty text for ${k}`);
    assert.deepEqual(placeholders(pt[k]), placeholders(en[k]), `placeholders of ${k}`);
  }
});

test("i18n: every key the page uses exists (static t(\"…\") calls and the dynamic families)", () => {
  const dict: any = NDP.i18n.DICT["pt-BR"];
  const used = new Set<string>();
  for (const f of readdirSync(WEB).filter((n) => n.endsWith(".js") && n !== "i18n.js")) {
    const src = readFileSync(WEB + f, "utf8");
    for (const m of src.matchAll(/\bt\(\s*"([^"]+)"/g)) if (!m[1]!.endsWith(".")) used.add(m[1]!);
    for (const m of src.matchAll(/"(name\.[a-z]+)"/g)) used.add(m[1]!);
  }
  // families built at run time
  for (const s of ["connecting", "pairing", "ready", "offline", "replaced"]) used.add(`state.${s}`);
  for (const m of ["READ_ONLY", "DEVELOPMENT"]) used.add(`mode.${m}`);
  const codec = readFileSync(WEB + "codec.js", "utf8");
  const statuses = /const Status = \{([^}]*)\}/s.exec(codec)![1]!.matchAll(/([A-Z_]+):/g);
  for (const s of statuses) if (s[1] !== "OK") used.add(`err.${s[1]!}`);
  const faq = /const SECTIONS = \[([^\]]*)\]/.exec(readFileSync(WEB + "faq.js", "utf8"))![1]!.matchAll(/"([a-z]+)"/g);
  for (const s of faq) { used.add(`faq.${s[1]!}.q`); used.add(`faq.${s[1]!}.a`); }
  for (const tab of ["files", "settings", "faq"]) used.add(tab);
  const missing = [...used].filter((k) => !(k in dict));
  assert.deepEqual(missing, [], "keys used by the page but missing from the dictionaries");
  assert.ok(used.size > 150, "the scan found suspiciously few keys");
});

test("i18n: t() substitutes, falls back to the key, and honours the chosen language", () => {
  const I = NDP.i18n;
  I.setLang("en");
  assert.equal(I.lang(), "en");
  assert.equal(I.t("usedOf", "1 MiB", "4 MiB"), "1 MiB of 4 MiB used");
  assert.equal(I.t("no.such.key"), "no.such.key");
  assert.ok(I.has("close") && !I.has("no.such.key"));
  I.setLang("pt-BR");
  assert.equal(I.t("close"), "Fechar");
  assert.equal(I.t("itemsCount", 3), "3 item(ns)");
  I.setLang("auto");
  assert.equal(I.choice(), "auto");
});
