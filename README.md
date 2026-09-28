# NSD Bridge

[Português (Brasil)](README.pt-BR.md)

**A wireless bridge to your Nintendo 3DS's SD card — from any browser, your terminal, or an AI coding assistant. Nothing hosted anywhere: it runs entirely on your own Wi-Fi.**

![License](https://img.shields.io/github/license/BC-Estrategias/nsd-bridge)
![Latest release](https://img.shields.io/github/v/release/BC-Estrategias/nsd-bridge)

> Drives **real hardware**. Writes modify a real SD card. Read the safety model below before you enable them.

## Install in 30 seconds

Requires a 3DS with [Luma3DS](https://github.com/LumaTeam/Luma3DS) and [FBI](https://github.com/Steveice10/FBI) installed. Open FBI → **Remote install via QR code / URL** and scan:

<img src="docs/images/install-qr.png" alt="Install QR code" width="220">

Or grab the `.cia` / `.3dsx` straight from the [latest release](https://github.com/BC-Estrategias/nsd-bridge/releases/latest). Either way, the file always matches whatever is currently released — no version number to keep track of.

## Three ways in, one console app

```
Browser  ⇄  the page served by the console itself, port 8080 (NDP over WebSocket)   ⇄  ┐
ndev CLI ⇄  NDP v1 over TCP/Wi-Fi                                                    ⇄  ┼  NSD Bridge (3DS app)
AI agent ⇄  MCP (stdio) ⇄ the same bridge, on your computer                          ⇄  ┘
```

### 🌐 The web page — the main event
Open `http://<console-ip>:8080` (the address is on the console's top screen) in any browser on your network. No install, nothing to configure:
- Browse, view images, edit text files (atomic save, optional `.bak`)
- **Drag and drop** to upload — files or whole folders, SHA-256 checked, conflicts handled
- Select, cut/copy/paste, *move to…* / *copy to…*, a **trash** (nothing is ever destroyed without a second, explicit step)
- See every device paired with the console — even ones offline right now — and forget any of them, right from the page
- Portuguese and English, light/dark, works fine on a phone
- Press **R** on the console to turn it off

### 💻 `ndev` — the CLI
`find` · `info` · `ls` · `cat` · `get` · `put` · `mkdir` · `mv` · `cp` · `rm` · `purge` · `pair` · `access`

### 🤖 MCP — for AI assistants
A server with 13 `nintendo_*` tools (Claude Code, Codex, …): device info, list/stat/read/write/mkdir/move/delete, upload/download to a local sandbox folder, agent log. Finds the console on its own when the IP changes (DHCP).

## Why it's safe to point an AI at your SD card
- **Starts READ_ONLY.** Press **X** on the console to allow writes. No network command can change the mode.
- **You choose the folders**, physically, on the console: **A** → *Access folders* → **Y** cycles closed → read → read+write. Nothing over the network can widen this.
- **System zones are never writable**, even inside an opened `/`: `/Nintendo 3DS`, `/luma` (except `/luma/plugins` and `/luma/titles`), `/boot.firm`, `/gm9`, `/private`, and the agent's own config.
- **Pairing required**, by comparing a 6-digit number shown on both screens (X25519, nobody in the middle can fake a match). The key never crosses the network. **The MCP server has no pairing tool** — an assistant cannot pair itself.
- **Deleting never destroys.** Items move to a trash first; only a person can empty it. The MCP server has no tool for that either.
- File contents handed to an assistant are treated as **untrusted data** (prompt-injection guidance ships in the tool descriptions).
- Everything is limited to the SD card. NAND is out of scope, by design.

Full details: [`ARCHITECTURE.md`](ARCHITECTURE.md) §4–5 and [`docs/protocol/ndp-v1.md`](docs/protocol/ndp-v1.md) §4, §11. Report security issues privately — see [`SECURITY.md`](SECURITY.md).

## What's next: DSi and Switch
The portable core (`agent/common`) was written platform-agnostic from day one — it already runs identically on the 3DS app and the desktop test agent. Adding a DSi or Switch target means writing a thin platform layer (network, filesystem, UI), not a rewrite. The 3DS is what's shipping today.

## Quick start (building from source)
Requirements: a 3DS with Luma3DS and the Homebrew Launcher (or FBI for a CIA), Wi-Fi, and Node ≥ 22.18 on your computer.

1. **Get the agent** onto the SD card: build it (below) or grab `nsd-bridge-vX.Y.Z.3dsx` / `.cia` from [releases](https://github.com/BC-Estrategias/nsd-bridge/releases/latest).
2. **Open it** on the 3DS — it shows its IP, the page address, and `Mode: READ_ONLY`.
3. **Pair** (once per computer): press **Y** on the console, then
   ```bash
   node bridge/packages/cli/src/main.ts pair <console-ip>
   ```
   It prints a 6-digit number; press **A** on the console when it shows the same one (**B** refuses).
3b. **Or from the browser**: open the page shown on the console, press **Y** on the console, click **Pair** and press **A** when the numbers match.
4. **Look around**: `node bridge/packages/cli/src/main.ts ls <console-ip> /3ds/nintendo-dev-agent`
5. **Open more folders** on the console (**A** → Access folders) and allow writes (**X**) only when you need them.
6. **Use it from an assistant** — see [`docs/mcp.md`](docs/mcp.md) for the Claude Code / Codex setup.

```bash
./scripts/check.sh --3ds      # vectors, C (-Werror, ASan/UBSan), TypeScript, and the 3DS agent (needs devkitPro)
./scripts/build-3ds-cia.sh    # a .cia with a Home Menu icon (needs third_party/bin/makerom + bannertool)
NDEV_DEV=1 ./scripts/build-3ds.sh   # developer build that starts with writes enabled
```
Try the protocol without a console: `./build/agent/host/ndp-host-agent --port 6464 -v --root /tmp/fake-sd` and point `ndev` at `127.0.0.1:6464`.

## Repository map
```
agent/common/   portable C99 core (frames, TLV, paths, policy, SHA/HMAC, pairing, folder list)
agent/posix/    sockets + file system backend shared by the 3DS agent and the host test agent
agent/3ds/      the console app (libctru) and its CIA description
agent/host/     the same core on macOS/Linux, for tests
web/            the page the console serves (plain JS/CSS, no build tools; scripts/build-web.py embeds it, gzipped)
bridge/         TypeScript: @ndev/core (codec, client, discovery), @ndev/cli, @ndev/mcp
docs/protocol/  NDP v1 specification + test vectors (generated by an independent Python implementation)
docs/research/  measurements, findings and the experiments that did not pan out
spike/          throwaway experiments (3GX plugin, Game Notes applet) — not part of the product
```

## Known limits
- The agent is a foreground app on the 3DS: opening a game closes it (Wi-Fi power-save adds ~100–200 ms after idle; creating a folder takes ~6 s).
- The web page is plain `http://` on your LAN: authenticated (per-frame HMAC) but **not encrypted**, like the CLI. Transfers run at the console's Wi-Fi speed (~0.6–1 MiB/s), one at a time.
- Not tested on Old 3DS, on Windows with a real console, or with other firmware setups.

## License
[Apache-2.0](LICENSE). See [`NOTICE`](NOTICE). Third-party tools used for optional builds (makerom, bannertool, CTRPluginFramework, 3gxtool) are downloaded by you into `third_party/`, which is not part of this repository.
