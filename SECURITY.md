# Security

## Threat model
The console and the computer are on the same LAN. The agent defends against **other devices on that LAN** and against an **assistant that only has the MCP tools**:
- nobody can read or write the SD card without having been paired by a person at the console (80-bit one-time code shown on the console; per-frame HMAC-SHA-256 with counters, so tampering and replay close the connection);
- nothing on the network can change the access mode, the opened folders or the pairings — those are changed with the console's buttons only;
- protected system zones are never writable, and deletes only move items to a trash folder.

Not protected against: someone who can read your computer's `~/.config/nintendo-dev/keys.json` (owner-only file, but any program running as you can read it, including an assistant with shell access); an eavesdropper on your LAN reading file contents (the channel is authenticated, **not encrypted**); physical access to the console or its SD card.

## Reporting a vulnerability
Please do not open a public issue. Contact the maintainers privately (BC Estratégias, via the repository's owner on GitHub) with the affected version and steps to reproduce. We aim to acknowledge within a few days.

## Verification done
The protocol core is tested by an independent Python reference (vectors), sanitizers (ASan/UBSan), mutation testing of the security-critical checks, and hardware tests on a New 3DS. This is not a formal audit.
