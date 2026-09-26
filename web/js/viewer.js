/* File viewer/editor: images, text (editable, saved atomically with an optional .bak), and a hex dump for anything else. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const { h, fill, icon, toast, dialog, confirmBox, saveBlob } = NDP.ui;
  const U = NDP.util, S = NDP.session;
  const t = (...a) => NDP.i18n.t(...a);

  const EDIT_MAX = 1024 * 1024, PEEK = 256 * 1024, IMAGE_MAX = 16 * 1024 * 1024, HEX_BYTES = 4096;

  function hexDump(bytes) {
    const rows = [];
    for (let o = 0; o < bytes.length; o += 16) {
      const chunk = bytes.subarray(o, o + 16);
      const hx = Array.from(chunk, (b) => b.toString(16).padStart(2, "0")).join(" ").padEnd(47, " ");
      const asc = Array.from(chunk, (b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : ".")).join("");
      rows.push(`${o.toString(16).padStart(8, "0")}  ${hx}  ${asc}`);
    }
    return rows.join("\n");
  }

  function validUtf8(bytes) {
    try { new TextDecoder("utf-8", { fatal: true }).decode(bytes); return true; } catch (_) { return false; }
  }

  async function open(path, entry) {
    const name = U.basename(path);
    const size = entry && entry.size !== undefined ? entry.size : (await S.run((c) => c.stat(path)).catch(() => ({ size: 0 }))).size;
    const writable = U.accessOf(path, S.access, S.info && S.info.mode) === "write";
    const body = h("div.viewer-body", null, h("div.empty", { text: t("loading") }));
    let dirty = false, getText = null;
    const box = () => document.querySelector(".dialog.viewer");

    dialog({
      title: name, wide: true, className: "viewer", body, buttons: [],
      onClose: () => {
        if (!dirty) return true;
        // the dialog helper wants an immediate answer: refuse now, close for real once the person confirms
        confirmBox(t("unsavedTitle"), t("unsavedBody"), t("discard"), true).then((yes) => { if (yes) { dirty = false; if (box()) box().close(undefined); } });
        return false;
      },
    });

    try {
      if (U.isImage(name) && size <= IMAGE_MAX) {
        const { data } = await S.run((c) => c.readBytes(path));
        const url = URL.createObjectURL(new Blob([data], { type: U.imageMime(name) }));
        fill(body, h("div.image-wrap", null, h("img", { src: url, alt: name, onload: (e) => { e.target.parentNode.dataset.dim = `${e.target.naturalWidth}×${e.target.naturalHeight}`; }, onerror: () => toast(t("cannotShowImage"), "error") })),
          toolbar([{ label: t("download"), icon: "download", run: () => saveBlob(new Blob([data]), name) }], `${U.formatSize(size)}`));
        return;
      }
      const wantText = U.isTextName(name) || size <= 64 * 1024;
      if (size > EDIT_MAX) {
        const { data } = await S.run((c) => c.readBytes(path, { length: PEEK }));
        const isBin = U.looksBinary(data) || !validUtf8(data.subarray(0, Math.max(0, data.length - 3)));
        fill(body, h("div.banner.info", null, icon("help"), h("p", { text: t("tooBigToEdit", U.formatSize(size), U.formatSize(PEEK)) })),
          h("pre.hex", { text: isBin ? hexDump(data.subarray(0, HEX_BYTES)) : new TextDecoder().decode(data) }),
          toolbar([{ label: t("download"), icon: "download", run: () => downloadAll(path, name) }], U.formatSize(size)));
        return;
      }
      const { data } = await S.run((c) => c.readBytes(path));
      if (!wantText || U.looksBinary(data) || !validUtf8(data)) {
        fill(body, h("div.banner.info", null, icon("help"), h("p", { text: t("binaryFile") })),
          h("pre.hex", { text: hexDump(data.subarray(0, HEX_BYTES)) }),
          toolbar([{ label: t("download"), icon: "download", run: () => saveBlob(new Blob([data]), name) }], U.formatSize(size)));
        return;
      }
      // ---- editor
      const original = new TextDecoder().decode(data);
      const bom = original.charCodeAt(0) === 0xfeff;
      const ta = h("textarea.editor", { value: bom ? original.slice(1) : original, spellcheck: false, wrap: "off", readOnly: !writable, "aria-label": name });
      const info = h("span.grow.muted");
      const backup = h("input", { type: "checkbox", checked: NDP.prefs.backupOnSave });
      const saveBtn = h("button.primary", { disabled: true, onclick: save }, icon("check"), t("save"));
      const status = () => {
        const v = ta.value;
        const lines = v === "" ? 0 : v.split("\n").length - (v.endsWith("\n") ? 1 : 0);
        info.textContent = `${lines} ${t("lines")} · ${new TextEncoder().encode(v).length} B${dirty ? " · " + t("modified") : ""}${writable ? "" : " · " + t("readOnlyFile")}`;
        saveBtn.disabled = !dirty || !writable;
      };
      getText = () => (bom ? "﻿" : "") + ta.value;
      ta.addEventListener("input", () => { dirty = true; status(); });
      ta.addEventListener("keydown", (e) => {
        if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "s") { e.preventDefault(); save(); }
        if (e.key === "Tab" && !e.shiftKey && writable) { e.preventDefault(); const s = ta.selectionStart; ta.setRangeText("\t", s, ta.selectionEnd, "end"); ta.dispatchEvent(new Event("input")); }
      });
      async function save() {
        if (!dirty || !writable) return;
        saveBtn.disabled = true;
        const bytes = new TextEncoder().encode(getText());
        try {
          await S.run((c) => c.write(path, bytes, { overwrite: true, backup: backup.checked }));
          dirty = false;
          status();
          toast(t("saved", name) + (backup.checked ? " " + t("backupKept", name + ".bak") : ""), "ok", 3500);
          if (NDP.files) NDP.files.refresh();
        } catch (e) { toast(t("saveFailed") + ": " + (e.statusName ? t("err." + e.statusName) : e.message), "error", 8000); saveBtn.disabled = false; }
      }
      fill(body, 
        writable ? null : h("div.banner.warn", null, icon("lock"), h("p", { text: t("readOnlyFileHow") })),
        ta,
        h("div.viewer-foot", null, info,
          h("label.check", null, backup, h("span", { text: t("keepBackup") })),
          h("button", { onclick: () => saveBlob(new Blob([getText()]), name) }, icon("download"), t("download")),
          saveBtn,
          h("button", { onclick: () => box() && box().close(undefined) }, t("close"))));
      status();
      setTimeout(() => ta.focus(), 0);
    } catch (e) {
      fill(body, h("div.banner.error", null, icon("warn"), h("p", { text: e.statusName ? t("err." + e.statusName) : e.message })));
      if (S.client && S.client.isClosed) S.connect();
    }
  }

  function toolbar(actions, info) {
    return h("div.viewer-foot", null, h("span.grow.muted", { text: info || "" }),
      actions.map((a) => h("button", { onclick: a.run }, icon(a.icon), a.label)),
      h("button", { onclick: () => document.querySelector(".dialog.viewer") && document.querySelector(".dialog.viewer").close(undefined) }, t("close")));
  }

  async function downloadAll(path, name) {
    const kill = toast(t("downloading", name), "info", 0);
    try {
      const col = U.blobCollector();
      await S.run((c) => c.read(path, { onProgress: (n, total) => kill.set(t("downloadingPct", name, total ? Math.round((100 * n) / total) : 0)) }, (b) => col.add(b)));
      saveBlob(col.blob(), name);
    } catch (e) { toast(e.message, "error"); }
    kill();
  }

  NDP.viewer = { open, hexDump };
})();
