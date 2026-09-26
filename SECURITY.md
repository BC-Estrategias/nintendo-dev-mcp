# Security

## Threat model
The console and the computer are on the same LAN. The agent defends against **other devices on that LAN** and against an **assistant that only has the MCP tools**:
- nobody can read or write the SD card without having been paired by a person at the console (the person presses A on the console after comparing a 6-digit number shown on both screens: an X25519 key agreement with a commitment, so the key never crosses the network and a man in the middle cannot fake a match; or the older 80-bit one-time code; then per-frame HMAC-SHA-256 with counters, so tampering and replay close the connection). Five refused requests close the pairing window;
- nothing on the network can change the access mode, the opened folders or the pairings — those are changed with the console's buttons only;
- protected system zones are never writable, and deletes only move items to a trash folder; permanent deletion (`FS_PURGE`) works only on what is already inside a trash, needs DEVELOPMENT mode, and is not available through the MCP server.

The **web page** (v1.2.0+) uses the same pairing and the same rules: a browser must be paired with the code shown on the console, the page cannot change the mode, the opened folders or the pairings, and it can be switched off with **R** on the console. Because it is plain `http://` on the LAN, the console rejects requests whose `Host` is not its own address (DNS rebinding) and WebSocket upgrades whose `Origin` is not the page itself (so a malicious website cannot drive it through your browser); it accepts only `GET`/`HEAD` with no body, serves a strict Content-Security-Policy, and caps frame and header sizes. The pairing key is kept in the browser's storage: anyone using that browser profile can use it (untick "Remember" on shared computers, or use *Forget this browser*).

Not protected against: someone who can read your computer's `~/.config/nintendo-dev/keys.json` (owner-only file, but any program running as you can read it, including an assistant with shell access); an eavesdropper on your LAN reading file contents (the channel is authenticated, **not encrypted**); physical access to the console or its SD card.

## Reporting a vulnerability
Please do not open a public issue. Contact the maintainers privately (BC Estratégias, via the repository's owner on GitHub) with the affected version and steps to reproduce. We aim to acknowledge within a few days.

## Verification done
The protocol core is tested by an independent Python reference (vectors), sanitizers (ASan/UBSan), mutation testing of the security-critical checks, and hardware tests on a New 3DS. This is not a formal audit.
