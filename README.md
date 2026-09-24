# Nintendo Dev MCP

Torna um Nintendo 3DS com CFW um target remoto de desenvolvimento controlável por agentes de IA (Codex, Claude Code) via MCP. Arquitetura pensada para expandir a DSi e Switch.

**Status:** M0 concluído (protocolo + núcleo testado no Mac, sem console). Próximo: M1 — agente 3DS com `PING` (primeiro teste no hardware).

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — arquitetura, segurança, roadmap
- [`docs/protocol/ndp-v1.md`](docs/protocol/ndp-v1.md) — especificação do protocolo NDP v1
- [`docs/research/findings.md`](docs/research/findings.md) — pesquisa (ftpd, sys-ftpd, libctru, Luma3DS, MCP)

## Estrutura
```
agent/common/   núcleo C99 sem dependências de SO (frames, TLV, paths, política, SHA/HMAC, HELLO/PING)
agent/host/     agente de teste para macOS/Linux (mesmo núcleo atrás de sockets POSIX)
bridge/         TypeScript: @ndev/core (codec, política, cliente TCP) e @ndev/cli (`ndev`)
docs/protocol/  especificação + vetores de teste (gerados por uma implementação Python independente)
tests/mcp-hello Compatibilidade MCP com Codex/Claude Code
```

## Desenvolvimento (macOS/Linux)
Requisitos: cmake, um compilador C, Node ≥ 22.18, Python 3.

```bash
./scripts/check.sh        # vetores em dia + build C (-Werror, ASan/UBSan) + testes C + tsc + testes TS
```

Experimentar sem o console:
```bash
./build/agent/host/ndp-host-agent --port 6464 -v &
node bridge/packages/cli/src/main.ts ping 127.0.0.1:6464
```

## Licença
[Apache-2.0](LICENSE). Veja `NOTICE`.
