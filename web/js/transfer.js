/* Copy / move between folders of the card, and the folder picker. Moving is a rename on the console (instant). Copying is
 * done BY the console (FS_COPY, one file at a time, in steps), so the data never travels through this browser; folders are
 * copied file by file (mkdir + copy), which is slow on the 3DS (creating a folder takes ~6 s). Nothing is ever overwritten:
 * a name clash is skipped, kept as "name (1)", or replaced after moving the old item to the trash. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const { h, icon, toast, dialog } = NDP.ui;
  const U = NDP.util, S = NDP.session;
  const t = (...a) => NDP.i18n.t(...a);

  const errText = (e) => {
    if (e && e.statusName) { const k = "err." + e.statusName; return NDP.i18n.has(k) ? t(k) : e.statusName; }
    return (e && e.message) || String(e);
  };
  const lower = (s) => s.replace(/[A-Z]/g, (c) => c.toLowerCase());

  // ------------------------------------------------------------------------------------------------ progress dialog
  function openJob(title) {
    const ctl = new AbortController();
    const line = h("p.job-line", { text: "…" });
    const bar = h("div.up-fill", { style: "width:0%" });
    const sub = h("p.muted.small.job-sub", { text: "" });
    let cancelled = false;
    dialog({ title, body: h("div.job", null, line, h("div.up-bar", null, bar), sub), buttons: [{ label: t("cancel"), value: "cancel" }] })
      .then(() => { cancelled = true; ctl.abort(); });
    const box = [...document.querySelectorAll(".backdrop .dialog")].pop();
    return {
      signal: ctl.signal,
      get cancelled() { return cancelled; },
      text(s) { line.textContent = s; },
      sub(s) { sub.textContent = s; },
      bar(frac) { bar.style.width = Math.max(0, Math.min(100, Math.round(frac * 100))) + "%"; },
      close() { if (box && box.isConnected) box.close(undefined); },
    };
  }

  // ------------------------------------------------------------------------------------------------ folder picker
  /** Lets the person browse folders and pick one. `okLabel` names the action; a folder is pickable when `canPick(path)`. */
  function pickFolder({ title, okLabel, start, canPick }) {
    let cwd = start || "/", token = 0;
    const crumbs = h("nav.crumbs");
    const list = h("div.picker-list");
    const ok = h("button.primary", { onclick: () => box && box.close(cwd) });
    const note = h("p.muted.small.picker-note");
    const up = h("button.icon-btn", { title: t("up"), onclick: () => go(U.parent(cwd)) }, icon("up"));
    let box = null;
    async function go(path) {
      const mine = ++token;
      cwd = path;
      list.replaceChildren(h("div.empty", { text: t("loading") }));
      let entries = [], err = null;
      try { entries = (await S.run((c) => c.list(path))).filter((e) => e.type === "dir" && (!e.name.startsWith(".") || e.name === ".ndp-trash") && e.name !== ".ndp-trash"); }
      catch (e) { err = e; }
      if (mine !== token) return;
      up.disabled = path === "/";
      crumbs.replaceChildren(...U.crumbs(path).map((c, i, a) => h("span.crumb-wrap", null, i ? h("span.sep", { text: "/" }) : null, h("button.crumb" + (i === a.length - 1 ? ".current" : ""), { onclick: () => go(c.path), text: c.name }))));
      list.replaceChildren(...(err ? [h("div.banner.error", null, icon("warn"), h("p", { text: errText(err) }))] : entries.length ? U.sortEntries(entries, "name", "asc").map((e) => h("button.picker-item", { onclick: () => go(U.join(path, e.name)) }, icon("folder", "ico dir"), h("span", { text: e.name }))) : [h("div.empty", { text: t("noSubfolders") })]));
      const allowed = canPick(path);
      ok.disabled = !allowed;
      ok.textContent = `${okLabel}: ${path === "/" ? "/" : U.basename(path)}`;
      note.textContent = allowed ? "" : t("cannotPickHere");
    }
    const p = dialog({ title, wide: false, className: "picker", body: h("div", null, h("div.picker-bar", null, up, crumbs), list, note, h("div.actions", null, ok)), buttons: [{ label: t("cancel"), value: null }] });
    box = [...document.querySelectorAll(".backdrop .dialog")].pop();
    go(cwd);
    return p.then((v) => (typeof v === "string" ? v : null));
  }

  // ------------------------------------------------------------------------------------------------ the engine
  async function listNames(dir) {
    return new Set((await S.run((c) => c.list(dir))).map((e) => lower(e.name)));
  }

  /** Every item below `dir` in copy order (a folder before what is inside it). */
  async function walk(srcDir, dstDir, out, job, depth) {
    if (job.cancelled) throw new Error("cancelled");
    if (depth > 24) throw new Error(t("tooDeep"));
    const kids = await S.run((c) => c.list(srcDir));
    for (const k of U.sortEntries(kids, "name", "asc")) {
      const s = U.join(srcDir, k.name), d = U.join(dstDir, k.name);
      if (k.type === "dir") { out.push({ op: "mkdir", src: s, dst: d, size: 0 }); await walk(s, d, out, job, depth + 1); }
      else out.push({ op: "copy", src: s, dst: d, size: k.size });
    }
  }

  const memo = { choice: null };
  async function askConflict(name) {
    if (memo.choice) return memo.choice;
    const all = h("input", { type: "checkbox" });
    const r = await dialog({
      title: t("alreadyExists"), body: h("div", null, h("p", { text: t("existsBody", name) }), h("label.check", null, all, h("span", { text: t("applyToAll") }))),
      buttons: [{ label: t("skip"), value: "skip" }, { label: t("keepBoth"), value: "both", primary: true }, { label: t("replaceOldToTrash"), value: "replace", danger: true }],
    });
    const choice = r || "skip";
    if (all.checked) memo.choice = choice;
    return choice;
  }

  /**
   * mode: "copy" | "move". entries: [{name, type, size}] living in srcDir; destDir is where they go.
   * Resolves with {ok, skipped, failed, cancelled}.
   */
  async function run({ mode, entries, srcDir, destDir }) {
    const res = { ok: 0, skipped: 0, failed: 0, cancelled: false };
    memo.choice = null;
    for (const e of entries) {
      if (e.type === "dir" && U.inside(destDir, U.join(srcDir, e.name))) { toast(t("cannotIntoItself", e.name), "error", 7000); return { ...res, failed: 1 }; }
    }
    if (mode === "move" && srcDir === destDir) { toast(t("alreadyHere"), "info", 3000); return res; }
    const title = mode === "move" ? t("moving") : t("copying");
    const job = openJob(title);
    try {
      let taken;
      try { taken = await listNames(destDir); } catch (e) { toast(errText(e), "error", 7000); job.close(); return { ...res, failed: 1 }; }

      // resolve names / conflicts first, so the plan (and the totals) is known before anything is touched
      const plan = [];
      for (const e of entries) {
        if (job.cancelled) { res.cancelled = true; break; }
        let name = e.name, replace = false;
        const same = mode === "copy" && srcDir === destDir; // pasting into the folder it came from = duplicate
        if (taken.has(lower(name)) || same) {
          if (same) name = U.uniqueName(name, taken);
          else {
            job.text(e.name);
            const c = await askConflict(e.name);
            if (c === "skip") { res.skipped++; continue; }
            if (c === "both") name = U.uniqueName(name, taken); else replace = true;
          }
        }
        taken.add(lower(name));
        plan.push({ e, name, replace });
      }

      let files = 0, bytes = 0;
      const steps = [];
      if (mode === "copy") {
        for (const p of plan) {
          const s = U.join(srcDir, p.e.name), d = U.join(destDir, p.name);
          if (p.e.type === "dir") {
            steps.push({ op: "mkdir", src: s, dst: d, size: 0, top: p });
            job.text(t("scanning", p.e.name));
            const from = steps.length;
            await walk(s, d, steps, job, 1);
            for (let k = from; k < steps.length; k++) steps[k].top = p; // what is inside belongs to the same top-level item
          } else steps.push({ op: "copy", src: s, dst: d, size: p.e.size, top: p });
        }
        for (const s of steps) if (s.op === "copy") { files++; bytes += s.size; }
        const folders = steps.filter((s) => s.op === "mkdir").length;
        if (folders > 4 || bytes > 64 * 1024 * 1024) {
          job.text(t("readyToCopy"));
          job.sub(t("copyPlan", files, folders, U.formatSize(bytes), Math.round(folders * 6 + bytes / (1024 * 1024) / 1.5)));
        }
      }

      let doneBytes = 0, doneFiles = 0;
      const t0 = performance.now();
      const guard = async (fn, label) => {
        try { await fn(); return true; }
        catch (err) {
          if (job.cancelled) return false;
          res.failed++;
          toast(`${label}: ${errText(err)}`, "error", 9000);
          if (S.client && S.client.isClosed) { await S.connect(); if (S.state !== "ready") throw err; }
          return false;
        }
      };

      if (mode === "move") {
        for (const p of plan) {
          if (job.cancelled) { res.cancelled = true; break; }
          job.text(p.e.name);
          job.bar(res.ok / Math.max(1, plan.length));
          const s = U.join(srcDir, p.e.name), d = U.join(destDir, p.name);
          const ok = await guard(async () => {
            if (p.replace) await S.run((c) => c.delete(d));
            await S.run((c) => c.rename(s, d));
          }, p.e.name);
          if (ok) res.ok++;
        }
      } else {
        const failedTops = new Set();
        const replaced = new Set();
        for (const st of steps) {
          if (job.cancelled) { res.cancelled = true; break; }
          if (failedTops.has(st.top)) continue;
          if (st.top.replace && !replaced.has(st.top)) { // the old item goes to the trash first (only once per top-level item)
            replaced.add(st.top);
            const d0 = U.join(destDir, st.top.name);
            if (!(await guard(() => S.run((c) => c.delete(d0)), st.top.name))) { failedTops.add(st.top); continue; }
          }
          job.text(U.basename(st.src));
          if (st.op === "mkdir") {
            const ok = await guard(() => S.run((c) => c.mkdir(st.dst)), U.basename(st.src));
            if (!ok) failedTops.add(st.top);
          } else {
            const base = doneBytes;
            const ok = await guard(() => S.run((c) => c.copy(st.src, st.dst, {
              signal: job.signal,
              onProgress: (n, tot) => {
                const done = base + n;
                job.bar(bytes ? done / bytes : (doneFiles + 1) / Math.max(1, files));
                const dt = (performance.now() - t0) / 1000;
                job.sub(`${U.formatSize(done)} / ${U.formatSize(bytes)} · ${U.formatRate(dt > 1 ? done / dt : 0)}`);
                void tot;
              },
            })), U.basename(st.src));
            if (ok) { doneBytes += st.size; doneFiles++; } else failedTops.add(st.top);
          }
        }
        res.ok = plan.length - failedTops.size;
      }
      if (job.cancelled) res.cancelled = true;
    } catch (err) {
      if (!job.cancelled) { res.failed++; toast(errText(err), "error", 9000); } else res.cancelled = true;
    } finally {
      job.close();
    }
    const msg = res.cancelled ? t("transferStopped") : res.failed ? t("transferErrors", res.ok, res.failed) : t(mode === "move" ? "movedN" : "copiedN", res.ok);
    toast(msg, res.failed || res.cancelled ? "error" : "ok", 5000);
    return res;
  }

  NDP.transfer = { run, pickFolder, openJob };
})();
