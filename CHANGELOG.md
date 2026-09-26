# Changelog

## 1.3.0 — first release with the web page
(1.2.0–1.2.2 were test builds that were never published; everything below is new in this release.)
- **Web page served by the console.** The 3DS app serves a file manager on port 8080 (address on the top screen; **R** turns it off/on): browse the SD card, view images, edit text files (atomic save, optional `.bak`), **drag and drop** files and folders to upload (SHA-256 checked by the console, conflict handling), download, plus Settings (device, SD and memory, opened folders, pairing, preferences) and an FAQ with the local MCP install steps. Portuguese and English, light/dark, phone friendly (one tap opens folders; checkboxes select). Same pairing, modes and folders as the CLI: the page cannot change any of them.
- **Organize:** checkbox selection, cut / copy / paste, *Move to…* / *Copy to…* with a folder picker, drag onto a folder to move, keyboard shortcuts, a **Trash** shortcut, restore, and **Delete forever / Empty trash**.
- New commands: `FS_RENAME` (0x0032, `ndev mv`, MCP `nintendo_fs_move`), `FS_PURGE` (0x0034, `ndev purge`; deletes for good but **only inside a trash**, in batches, DEVELOPMENT mode; not exposed through the MCP), `FS_COPY` (0x0035, `ndev cp`; the console copies one file in bounded steps and renames it into place, never overwrites; folders are copied file by file by the page).
- NDP over WebSocket (spec §15) with Host/Origin checks against DNS rebinding and cross-site use; the raw NDP client and the page are independent, so the MCP server keeps working while the page is open. Two tabs no longer fight over the connection (the replaced one is told, WebSocket close 4001).
- **The pairing store never says "full":** it now holds 8 pairings, a new pairing with the same name (the same browser pairing again) replaces its old key, and when it is full the oldest one is dropped (the console names it in the prompt before you press A). Before, a full store refused everything until SELECT×2 wiped all of them.
- **Pairing by number comparison** (`PAIR_BEGIN`/`REVEAL`/`POLL`): press Y on the console, click *Pair* on the page (or run `ndev pair`), and press **A** on the console when both screens show the same 6-digit number. No code to type. X25519 + a commitment (so the number cannot be ground by an attacker); the key never crosses the network. Implemented three times (C, TypeScript with node:crypto, the page in BigInt JavaScript) and checked against RFC 7748 and an independent Python reference. The 16-character code still works (`ndev pair --code`).
- The page ships its own SHA-256/HMAC (plain `http://` has no `crypto.subtle`), verified against the same test vectors as the C and TypeScript implementations. A refused stored key is no longer deleted from the browser; the pairing code field formats itself and pairs as soon as it is complete.
- Large downloads are assembled in 8 MiB blocks (the browser can keep them on disk) with a percentage; uploads warn about files of 4 GiB or more (FAT32) and about not enough free space.

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
