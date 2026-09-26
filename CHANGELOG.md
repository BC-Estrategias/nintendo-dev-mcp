# Changelog

## 1.2.2
- **Delete forever / Empty trash.** New command `FS_PURGE` (0x0034): permanently deletes items that are **inside a trash folder** (or empties it), only in DEVELOPMENT mode and only in writable folders; it works in small batches so a huge trash never keeps the console busy, and the page shows the count with a Cancel button. Also `ndev purge`. The MCP server does **not** get it: an assistant still cannot destroy data.
- Large downloads no longer sit in memory as one big list: the page assembles them in 8 MiB blocks (the browser can keep those on disk) and shows the percentage.

## 1.2.1
- Two browser tabs (or two browsers) no longer fight over the console: the console has one page connection, and the page that gets replaced is told (WebSocket close 4001) and shows "opened somewhere else" with a *Use this tab* button, instead of reconnecting and evicting the other one every few seconds.
- 1.2.0 was a test build that was never published; everything below shipped first in 1.2.1.

## 1.2.0
- **Web page served by the console.** The 3DS app now serves a file manager on port 8080 (address on the top screen; **R** turns it off/on): browse the SD card, view images, edit text files (atomic save, optional `.bak`), **drag and drop** files and folders to upload (SHA-256 checked by the console, conflict handling), download, rename/move, move to the trash and restore, plus Settings (device, SD and memory, opened folders, pairing, preferences) and an FAQ with the local MCP install steps. Portuguese and English. Same pairing, same modes and same folders as the CLI: the page cannot change any of them.
- New command `FS_RENAME` (0x0032): move/rename without ever overwriting (also `ndev mv` and the MCP tool `nintendo_fs_move`).
- NDP over WebSocket (spec §15) with Host/Origin checks against DNS rebinding and cross-site use; the raw NDP client and the page are independent, so the MCP server keeps working while the page is open.
- The page ships its own SHA-256/HMAC (plain `http://` has no `crypto.subtle`), verified against the same test vectors as the C and TypeScript implementations.

## 1.1.1
- The 3DS agent no longer reports `app_mem_*` (it always read 0 free of the agent's own 64 MB mode, which is misleading). Found on the first hardware run of `DEVICE_INFO`.

## 1.1.0
- `DEVICE_INFO` (0x0010): console model, firmware, RAM, memory regions and SD card total/free space. `ndev info`, and `nintendo_device_info` now returns them (fields the console cannot measure are absent, never guessed).

## 1.0.0 — 2026-09-25
First release for other people.
- The agent **starts READ_ONLY** (press X on the console to allow writes). `NDEV_DEV=1` builds a developer flavour.
- Pairing with per-frame HMAC, folders chosen on the console (default: only `/3ds/nintendo-dev-agent`), `ACCESS_INFO`, `/luma/plugins` and `/luma/titles` writable exceptions inside the protected `/luma`.
- CIA build (`scripts/build-3ds-cia.sh`), CLI `ndev pair/access`, MCP `nintendo_device_info` lists the opened folders.
- CI: Linux case-sensitivity test fixed; retries for the devkitPro package download.
- Research kept for reference: 3GX plugin inside a game and Game Notes applet replacement (see `docs/research/`); neither ships.

## 0.6.x
Owner-chosen folders (0.6.0), stack-usage fix (0.6.1), `/luma/plugins` + `/luma/titles` exceptions (0.6.2), menu shows the effective level (0.6.3).

## 0.5.0
Pairing and authenticated frames.

## 0.1–0.4
Protocol core, read-only file access, atomic writes, trash, MCP server.
