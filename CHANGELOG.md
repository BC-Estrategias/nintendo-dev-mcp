# Changelog

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
