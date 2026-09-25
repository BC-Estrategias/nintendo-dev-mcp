# Servidor MCP (`@ndev/mcp`)

Expõe o console 3DS (via o agente) como ferramentas MCP por **stdio**. Usa `@modelcontextprotocol/server` 2.x com `serveStdio`, que atende clientes das duas eras (Claude Code 2.1.177 e Codex 0.134 abrem com `initialize`).

## Registrar
Node ≥ 22.18 (roda o TypeScript direto). Ajuste os caminhos; `--local-root` é a(s) pasta(s) **deste computador** que as ferramentas de upload/download podem ler/escrever (padrão: o diretório de trabalho do servidor).

```bash
# Claude Code
claude mcp add nintendo -- node /Users/macmini/nintendo-dev-mcp/bridge/packages/mcp/src/main.ts \
  --local-root /Users/macmini/nintendo-dev-mcp/dist

# Codex
codex mcp add nintendo -- node /Users/macmini/nintendo-dev-mcp/bridge/packages/mcp/src/main.ts \
  --local-root /Users/macmini/nintendo-dev-mcp/dist
```
Remover: `claude mcp remove nintendo` / `codex mcp remove nintendo`.

O endereço do console **não precisa** ser configurado: o servidor usa `--host`/`$NDEV_HOST` se houver; senão o último endereço conhecido; senão **varre as sub-redes deste computador** procurando o agente (o IP do 3DS muda por DHCP). Descoberta manual: `ndev find`.

## Ferramentas
| Ferramenta | Faz | Muda o SD? |
|---|---|---|
| `nintendo_find_device` | procura o console na rede | não |
| `nintendo_ping`, `nintendo_device_info` | alcançabilidade, modo, versões (modelo/memória/SD **indisponíveis** até existir `DEVICE_INFO`) | não |
| `nintendo_fs_list`, `nintendo_fs_stat` | listar / tipo+tamanho | não |
| `nintendo_fs_read` | lê bytes (utf8/base64/hex, `offset`/`length`/`tail`, teto 256 KiB por chamada) | não |
| `nintendo_agent_log` | fim do log do próprio agente | não |
| `nintendo_fs_download` | copia arquivo do SD para uma pasta local permitida, verificando SHA-256 | não (grava local) |
| `nintendo_fs_write` | grava arquivo (atômico, hash verificado, **nunca sobrescreve em silêncio**) | **sim** |
| `nintendo_fs_upload` | envia arquivo local (ex.: `.3dsx`) | **sim** |
| `nintendo_fs_mkdir` | cria pasta (o `mkdir` do console leva ~6 s) | **sim** |
| `nintendo_fs_delete` | **move para a lixeira** (`<pasta liberada>/.ndp-trash/`), nada é destruído | **sim** |

## Segurança
- As descrições das ferramentas dizem que é hardware real, que escritas modificam o SD e que o conteúdo de arquivos é **dado não confiável** (proteção contra injeção de prompt).
- Anotações MCP: leituras `readOnlyHint`; escritas/delete `destructiveHint` (o cliente deve pedir confirmação). As anotações **não são a segurança**: a política real é a do agente (modo, pastas liberadas, zonas protegidas).
- **Sandbox local:** upload/download só dentro de `--local-root` (resolve symlinks e `..`); nenhum arquivo local é sobrescrito sem `overwrite_local=true`.
- **Auditoria:** `~/.config/nintendo-dev/audit.log` (JSON lines: quando, ferramenta, caminhos, ok/status, ms — **nunca o conteúdo**). `--no-audit` desliga.
- Escritas e deletes **nunca são repetidos** automaticamente após uma falha de conexão (o resultado seria ambíguo); leituras são repetidas uma vez.
- Sem pareamento ainda: com o modo `DEVELOPMENT` ligado no console (build de desenvolvimento), qualquer aparelho da rede pode escrever na pasta liberada. Ver `ARCHITECTURE.md` §5.

## Testes
`bridge/packages/mcp/test/mcp.test.ts` (13 testes) usa um cliente stdio roteirizado com os handshakes exatos do Claude Code e do Codex, contra o núcleo C real (agente de teste), incluindo um agente falso que cai no meio de um pedido para provar que escritas não são repetidas.
