# Nintendo Dev MCP (pt-BR)

[English README](README.md)

Torna um Nintendo 3DS com CFW um target remoto de desenvolvimento controlável por agentes de IA (Codex, Claude Code) via MCP. Arquitetura pensada para expandir a DSi e Switch.

**Status (v1.0.0):** pronto para uso e distribuição. Agente 3DS (leitura, escrita atômica, lixeira, pareamento com HMAC por frame, pastas liberadas pelo dono no próprio console), CLI `ndev` e servidor MCP, validados num New 3DS com Luma3DS 13.3.3. O agente abre em **READ_ONLY**. Pesquisas que não seguiram (plugin 3GX dentro de jogo, agente no lugar do Notas de jogo) estão em `docs/research/` com o que se aprendeu.

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — arquitetura, segurança, roadmap
- [`docs/protocol/ndp-v1.md`](docs/protocol/ndp-v1.md) — especificação do protocolo NDP v1
- [`docs/research/findings.md`](docs/research/findings.md) — pesquisa (ftpd, sys-ftpd, libctru, Luma3DS, MCP)

## Estrutura
```
agent/common/   núcleo C99 sem dependências de SO (frames, TLV, paths, política, SHA/HMAC, HELLO/PING)
agent/host/     agente de teste para macOS/Linux (mesmo núcleo atrás de sockets POSIX)
bridge/         TypeScript: @ndev/core (codec, cliente TCP, descoberta), @ndev/cli (`ndev`) e @ndev/mcp (servidor MCP)
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
