/* The page's connection to the console: connect, log in with the key stored in this browser (or pair), keep the socket
 * alive, reconnect. Keys live in the browser's own storage (localStorage, or sessionStorage when the person does not
 * want to be remembered) and are never sent anywhere: only proofs derived from them are. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const C = NDP.codec, A = NDP.auth;

  const KEY = "ndev.keys";
  const store = {
    read() {
      const out = {};
      for (const s of [globalThis.localStorage, globalThis.sessionStorage]) {
        try { Object.assign(out, JSON.parse((s && s.getItem(KEY)) || "{}")); } catch (_) { /* unavailable or corrupt: none */ }
      }
      return out;
    },
    save(deviceId, entry, persistent) {
      const s = persistent ? globalThis.localStorage : globalThis.sessionStorage;
      try {
        const cur = JSON.parse(s.getItem(KEY) || "{}");
        cur[deviceId] = entry;
        s.setItem(KEY, JSON.stringify(cur));
        return true;
      } catch (_) { return false; }
    },
    forget(deviceId) {
      for (const s of [globalThis.localStorage, globalThis.sessionStorage]) {
        try {
          const cur = JSON.parse(s.getItem(KEY) || "{}");
          delete cur[deviceId];
          s.setItem(KEY, JSON.stringify(cur));
        } catch (_) { /* ignore */ }
      }
    },
  };

  const listeners = {};
  const on = (ev, fn) => { (listeners[ev] = listeners[ev] || []).push(fn); };
  const emit = (ev, ...a) => { for (const fn of listeners[ev] || []) { try { fn(...a); } catch (e) { console.error(e); } } };

  const S = {
    state: "connecting", // connecting | pairing | ready | offline | replaced
    client: null, info: null, access: null, device: null, error: null, lastPingMs: null, deviceHex: null,
    url: () => `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`,
    on, store,
  };
  let pingTimer = null, retryTimer = null, attempt = 0, connecting = null;

  function setState(state, error) {
    S.state = state;
    S.error = error || null;
    emit("state", state);
  }

  async function loadContext() {
    const c = S.client;
    try { S.access = await c.accessInfo(); } catch (e) { S.access = null; }
    try { S.device = await c.deviceInfo(); } catch (e) { S.device = null; }
    try { S.lastPingMs = await c.ping(); } catch (_) { /* ignore */ }
    emit("context");
  }

  function startKeepAlive() {
    clearInterval(pingTimer);
    // the console drops idle clients after 120 s: a ping every 25 s while nothing else is going on
    pingTimer = setInterval(async () => {
      if (S.state !== "ready" || !S.client || S.client.busy) return;
      try { S.lastPingMs = await S.client.ping(); emit("ping"); } catch (_) { /* the close handler reconnects */ }
    }, 25000);
  }

  async function connect() {
    if (connecting) return connecting;
    connecting = (async () => {
      clearTimeout(retryTimer);
      setState("connecting");
      if (S.client) { S.client.onclose = null; S.client.close(); }
      const c = new NDP.NdpWsClient(S.url());
      S.client = c;
      try {
        await c.open();
        S.info = await c.hello();
        S.deviceHex = S.info.deviceId ? C.hex(S.info.deviceId) : "console";
        c.onclose = () => {
          if (S.client !== c || S.state === "offline" || S.state === "replaced") return;
          clearInterval(pingTimer);
          if (c.replaced) { setState("replaced", null); return; } // another page took the console: do not fight for it
          setState("offline", null);
          scheduleReconnect();
        };
        if (S.info.auth === "required") {
          const k = store.read()[S.deviceHex];
          if (!k) { setState("pairing"); return; }
          try {
            await c.authenticate(C.fromHex(k.psk));
          } catch (e) {
            // The key is NOT deleted: a refusal can be transient (the console just restarted, or the pairings were cleared and
            // this browser will be paired again, which replaces the key). Deleting it here made a valid key vanish.
            if (e.statusName === "UNAUTHORIZED") { console.warn("the console refused the stored key", e); setState("pairing", "rejected"); return; }
            throw e;
          }
        }
        attempt = 0;
        setState("ready");
        startKeepAlive();
        loadContext();
      } catch (e) {
        setState("offline", e);
        scheduleReconnect();
      }
    })().finally(() => { connecting = null; });
    return connecting;
  }

  function scheduleReconnect() {
    clearTimeout(retryTimer);
    const wait = Math.min(15000, 1500 * 2 ** Math.min(attempt++, 4));
    retryTimer = setTimeout(connect, wait);
    emit("retry", wait);
  }

  /** Pairs this browser with the console (its pairing window must be open) and logs in. */
  async function pair(codeText, label, remember) {
    const code = A.codeDecode(codeText);
    if (!code) throw new C.NdpError(C.Status.BAD_REQUEST, "code");
    const c = S.client;
    const { psk } = await c.pair(code, label);
    if (!store.save(S.deviceHex, { psk: C.hex(psk), label, at: Date.now() }, remember)) throw new Error("this browser cannot store the key");
    await connect(); // a new connection: the console closes its pairing window after one pairing
  }

  /** Pairing by number comparison: `onNumber` shows the number; the person confirms on the console. */
  async function pairByNumber(label, remember, onNumber, signal) {
    const c = S.client;
    const { psk } = await c.pairByNumber(label, onNumber, { signal });
    if (!store.save(S.deviceHex, { psk: C.hex(psk), label, at: Date.now() }, remember)) throw new Error("this browser cannot store the key");
    await connect();
  }

  function forgetThisBrowser() {
    store.forget(S.deviceHex);
    return connect();
  }

  /** Runs `fn(client)` when ready. Reads may be retried once after a reconnect; writes never are. */
  async function run(fn, { retry = false } = {}) {
    if (S.state !== "ready") throw new C.NdpTransportError("not connected");
    try {
      return await fn(S.client);
    } catch (e) {
      if (retry && e instanceof C.NdpTransportError && S.client.isClosed) {
        await connect();
        if (S.state === "ready") return fn(S.client);
      }
      throw e;
    }
  }

  S.hasStoredKey = () => !!(S.deviceHex && store.read()[S.deviceHex]);
  Object.assign(S, { connect, pair, pairByNumber, forgetThisBrowser, run, refreshContext: loadContext });
  NDP.session = S;
})();
