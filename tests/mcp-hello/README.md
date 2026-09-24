# mcp-hello — testes de compatibilidade MCP

Não faz parte do produto. Serve para decidir o SDK do Bridge.

- `probe.mjs` — servidor stdio sem SDK que **registra** cada mensagem recebida (`PROBE_LOG=arquivo`). Mostra a revisão do protocolo que o cliente fala.
- `hello-v2.mjs` — servidor com `@modelcontextprotocol/server` 2.1.0 (`serveStdio`, atende as duas eras).
- `drive.mjs` — cliente roteirizado com o handshake exato capturado dos clientes reais: `node drive.mjs ./hello-v2.mjs claude|codex`.

## Resultado (2026-09-24)
| Cliente | Versão | Abertura observada |
|---|---|---|
| Claude Code | 2.1.177 | `initialize`, protocolVersion **2025-11-25** |
| Codex | 0.134.0 | `initialize`, protocolVersion **2025-06-18** |

Nenhum fala a spec 2026-07-28. O `hello-v2` respondeu corretamente a ambos (initialize, tools/list, tools/call).

**Não testado ponta a ponta:** a chamada de tool *pelo modelo* nos clientes reais. No teste automático, o `claude -p` aninhado falhou por token OAuth expirado e o `codex exec` falhou porque o modelo padrão exige uma versão mais nova do Codex. Só o handshake e o `tools/list` reais foram observados.

## Teste real (manual, opcional)
```bash
claude mcp add hello-v2 -- node /Users/macmini/nintendo-dev-mcp/tests/mcp-hello/hello-v2.mjs
# em uma sessão do Claude Code: "chame a tool hello com text=ola"
claude mcp remove hello-v2

codex mcp add hello-v2 -- node /Users/macmini/nintendo-dev-mcp/tests/mcp-hello/hello-v2.mjs
codex mcp remove hello-v2
```
