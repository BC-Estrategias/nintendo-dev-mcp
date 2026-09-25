# Changelog

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
