/* Small DOM toolkit: element builder, toasts, dialogs, context menu, icons. Everything user-controlled (file names, error
 * text from the console) goes through textContent, never innerHTML. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const t = (...a) => NDP.i18n.t(...a);

  /** h("div.cls#id", {onclick, title, dataset:{}}, child, "text", [children]) */
  function h(spec, props, ...children) {
    const m = /^([a-z0-9]+)((?:[.#][\w-]+)*)$/i.exec(spec);
    const el = document.createElement(m ? m[1] : "div");
    if (m && m[2]) for (const part of m[2].match(/[.#][\w-]+/g)) { if (part[0] === ".") el.classList.add(part.slice(1)); else el.id = part.slice(1); }
    if (props) {
      for (const [k, v] of Object.entries(props)) {
        if (v === undefined || v === null || v === false) continue;
        if (k === "dataset") Object.assign(el.dataset, v);
        else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
        else if (k === "class") v.split(" ").filter(Boolean).forEach((c) => el.classList.add(c));
        else if (k === "text") el.textContent = v;
        else if (k in el && k !== "list") el[k] = v;
        else el.setAttribute(k, v === true ? "" : v);
      }
    }
    for (const c of children.flat(Infinity)) {
      if (c === undefined || c === null || c === false) continue;
      el.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return el;
  }

  /** replaceChildren that skips null/undefined/false (replaceChildren would print them as text). */
  function fill(el, ...kids) {
    el.replaceChildren(...kids.flat(Infinity).filter((k) => k !== null && k !== undefined && k !== false));
  }

  const ICONS = {
    folder: "M3 6a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z",
    file: "M6 3h8l5 5v13H6zM14 3v5h5",
    image: "M4 5h16v14H4zM8 10a1.5 1.5 0 1 0 0-.1M4 17l5-5 4 4 3-3 4 4",
    up: "M12 19V6M6 12l6-6 6 6",
    refresh: "M20 12a8 8 0 1 1-2.3-5.6M20 4v5h-5",
    upload: "M12 16V4M6 10l6-6 6 6M4 20h16",
    download: "M12 4v12M6 10l6 6 6-6M4 20h16",
    trash: "M5 7h14M9 7V4h6v3M7 7l1 13h8l1-13",
    edit: "M4 20h4L19 9l-4-4L4 16zM14 6l4 4",
    plus: "M12 5v14M5 12h14",
    newfile: "M6 3h8l5 5v13H6zM14 3v5h5M12 12v6M9 15h6",
    lock: "M6 11h12v9H6zM8 11V8a4 4 0 0 1 8 0v3",
    close: "M6 6l12 12M18 6L6 18",
    check: "M5 12l5 5 9-10",
    warn: "M12 4l9 16H3zM12 10v5M12 17.5v.1",
    gear: "M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM4 12h2M18 12h2M12 4v2M12 18v2M6.3 6.3l1.4 1.4M16.3 16.3l1.4 1.4M6.3 17.7l1.4-1.4M16.3 7.7l1.4-1.4",
    help: "M9.5 9a2.5 2.5 0 1 1 3.6 2.2c-.7.4-1.1 1-1.1 1.8M12 17.5v.1M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z",
    copy: "M8 8h11v12H8zM5 16V4h11",
    restore: "M4 12a8 8 0 1 1 2.3 5.6M4 20v-5h5",
    console: "M4 5h16v14H4zM4 12h16M8 8.5h.1M8 15.5h.1",
  };
  function icon(name, cls) {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("width", "18");
    svg.setAttribute("height", "18");
    svg.setAttribute("fill", "none");
    svg.setAttribute("stroke", "currentColor");
    svg.setAttribute("stroke-width", "1.8");
    svg.setAttribute("stroke-linecap", "round");
    svg.setAttribute("stroke-linejoin", "round");
    svg.setAttribute("aria-hidden", "true");
    if (cls) svg.setAttribute("class", cls);
    const p = document.createElementNS("http://www.w3.org/2000/svg", "path");
    p.setAttribute("d", ICONS[name] || ICONS.file);
    svg.append(p);
    return svg;
  }

  // ---- toasts
  function toast(message, kind = "info", ms = 5000) {
    let host = document.getElementById("toasts");
    if (!host) { host = h("div#toasts", { role: "status", "aria-live": "polite" }); document.body.append(host); }
    const el = h("div.toast." + kind, null, icon(kind === "error" ? "warn" : kind === "ok" ? "check" : "help"), h("span", { text: message }));
    host.append(el);
    const kill = () => el.remove();
    el.addEventListener("click", kill);
    if (ms) setTimeout(kill, ms);
    kill.set = (text) => { const s = el.querySelector("span"); if (s) s.textContent = text; };
    return kill;
  }

  // ---- modal dialog: resolves with the value of the pressed button (or undefined on Esc / backdrop)
  function dialog({ title, body, buttons, wide, onClose, className }) {
    return new Promise((resolve) => {
      const prev = document.activeElement;
      const back = h("div.backdrop");
      const box = h("div.dialog" + (wide ? ".wide" : "") + (className ? "." + className : ""), { role: "dialog", "aria-modal": "true", "aria-label": title || "" });
      const done = (v) => {
        if (onClose && onClose(v) === false) return;
        document.removeEventListener("keydown", onKey, true);
        back.remove();
        if (prev && prev.focus) prev.focus();
        resolve(v);
      };
      const onKey = (e) => {
        if (e.key === "Escape") { e.stopPropagation(); done(undefined); }
        if (e.key === "Enter" && e.target.tagName !== "TEXTAREA" && e.target.tagName !== "BUTTON") {
          const primary = (buttons || []).find((b) => b.primary);
          if (primary) { e.preventDefault(); e.stopPropagation(); done(primary.value); }
        }
      };
      document.addEventListener("keydown", onKey, true);
      back.addEventListener("mousedown", (e) => { if (e.target === back) done(undefined); });
      const head = title ? h("div.dialog-head", null, h("h2", { text: title }), h("button.icon-btn", { title: t("close"), onclick: () => done(undefined) }, icon("close"))) : null;
      const foot = buttons && buttons.length ? h("div.dialog-foot", null, buttons.map((b) => h("button" + (b.primary ? ".primary" : "") + (b.danger ? ".danger" : ""), { onclick: () => done(b.value), text: b.label }))) : null;
      fill(box, head, h("div.dialog-body", null, body), foot);
      back.append(box);
      document.body.append(back);
      const first = box.querySelector(".dialog-body input, .dialog-body textarea") || box.querySelector("button.primary") || box.querySelector(".dialog-foot button, button");
      if (first) setTimeout(() => first.focus(), 0);
      box.close = done;
    });
  }

  const confirmBox = (title, message, okLabel, danger) =>
    dialog({ title, body: h("p", { text: message }), buttons: [{ label: t("cancel"), value: false }, { label: okLabel || t("ok"), value: true, primary: true, danger }] });

  async function promptBox(title, label, value, validate) {
    const input = h("input", { type: "text", value: value || "", spellcheck: false, autocapitalize: "off", autocomplete: "off" });
    const err = h("div.field-error", { role: "alert" });
    setTimeout(() => { const i = input.value.lastIndexOf("."); input.focus(); input.setSelectionRange(0, i > 0 ? i : input.value.length); }, 10);
    for (;;) {
      const r = await dialog({
        title, body: h("label.field", null, h("span", { text: label }), input, err),
        buttons: [{ label: t("cancel"), value: false }, { label: t("ok"), value: true, primary: true }],
      });
      if (!r) return null;
      const v = input.value.trim();
      const problem = validate ? validate(v) : null;
      if (!problem) return v;
      err.textContent = problem;
    }
  }

  // ---- context menu
  function contextMenu(x, y, items) {
    closeMenu();
    const menu = h("div.menu", { role: "menu" });
    for (const it of items) {
      if (it === "-") { menu.append(h("div.menu-sep")); continue; }
      menu.append(h("button.menu-item" + (it.danger ? ".danger" : ""), { role: "menuitem", disabled: !!it.disabled, onclick: () => { closeMenu(); it.run(); } }, it.icon ? icon(it.icon) : h("span.icon-gap"), h("span", { text: it.label })));
    }
    document.body.append(menu);
    const r = menu.getBoundingClientRect();
    menu.style.left = Math.max(4, Math.min(x, innerWidth - r.width - 4)) + "px";
    menu.style.top = Math.max(4, Math.min(y, innerHeight - r.height - 4)) + "px";
    setTimeout(() => {
      document.addEventListener("mousedown", closeOnOutside, true);
      document.addEventListener("keydown", closeOnKey, true);
      window.addEventListener("blur", closeMenu);
      window.addEventListener("resize", closeMenu);
    }, 0);
    function closeOnOutside(e) { if (!menu.contains(e.target)) closeMenu(); }
    function closeOnKey(e) { if (e.key === "Escape") closeMenu(); }
    menu._cleanup = () => {
      document.removeEventListener("mousedown", closeOnOutside, true);
      document.removeEventListener("keydown", closeOnKey, true);
      window.removeEventListener("blur", closeMenu);
      window.removeEventListener("resize", closeMenu);
    };
  }
  function closeMenu() {
    for (const m of document.querySelectorAll(".menu")) { if (m._cleanup) m._cleanup(); m.remove(); }
  }

  function copyText(text) {
    const done = () => toast(t("copied"), "ok", 1800);
    if (navigator.clipboard && globalThis.isSecureContext) { navigator.clipboard.writeText(text).then(done, () => fallback()); return; }
    fallback();
    function fallback() {
      const ta = h("textarea", { value: text, style: "position:fixed;left:-1000px;top:0" });
      document.body.append(ta);
      ta.select();
      try { document.execCommand("copy"); done(); } catch (_) { toast(t("copyFailed"), "error"); }
      ta.remove();
    }
  }

  function saveBlob(blob, name) {
    const url = URL.createObjectURL(blob);
    const a = h("a", { href: url, download: name, style: "display:none" });
    document.body.append(a);
    a.click();
    setTimeout(() => { a.remove(); URL.revokeObjectURL(url); }, 30000);
  }

  NDP.ui = { h, fill, icon, toast, dialog, confirmBox, promptBox, contextMenu, closeMenu, copyText, saveBlob };
})();
