# Nintendo Dev MCP — Arquitetura (rascunho v0.1)

Status: **proposta para revisão**. Nenhum código de produto escrito ainda. Fatos citados aqui estão verificados (ou marcados como hipótese) em [`docs/research/findings.md`](docs/research/findings.md).

## 1. Objetivo

Transformar um Nintendo 3DS com CFW num **target remoto de desenvolvimento** controlável por agentes de IA (Codex, Claude Code…) via MCP. O 3DS expõe hardware/filesystem por um protocolo leve; um **Bridge** no seu computador (Mac/Windows/Linux, mesma LAN do console) fala esse protocolo e vira servidor MCP. Nenhum LLM roda no console.

O projeto também deve poder ser **distribuído a outras pessoas** (ver §13): instalação simples em Mac/Windows/Linux, padrões seguros e nada específico da máquina do autor.

Princípios: incremental e provado em hardware; protocolo neutro de plataforma (3DS → DSi → Switch); agente do console simples e sem dependências pesadas; segurança desde o primeiro commit (isto é escrita remota no SD).

## 2. Visão geral

```
┌──────────────────────────────────────────────────────────────────┐
│ Clientes                                                         │
│  Codex / Claude Code (MCP stdio ou HTTP local)                   │
│  Você (app desktop tipo FileZilla, fase futura)                  │
└───────────────┬──────────────────────────────────────────────────┘
                │
┌───────────────▼──────────────────────────────────────────────────┐
│ BRIDGE (TypeScript) — "nintendo-dev"                             │
│                                                                  │
│  Front-ends (finos)        Núcleo (biblioteca)                   │
│  ├ mcp-stdio               ├ Device interface (NintendoDevice)   │
│  ├ mcp-http (localhost)    ├ Policy: modos, paths, confirmações  │
│  ├ cli (`ndev`)            ├ Audit log (toda ação)               │
│  ├ desktop UI  (depois)    ├ Codec do protocolo NDP + transporte │
│                            └ Adaptadores de plataforma           │
│                              (paths conhecidos, crashes, info)   │
└───────────────┬──────────────────────────────────────────────────┘
                │ NDP v1 sobre TCP (LAN), autenticado por HMAC
┌───────────────▼──────────────────────────────────────────────────┐
│ AGENT (C99 no core comum; libctru no alvo 3DS)                   │
│  agent/common : frames, TLV, dispatch, política de paths, SHA/HMAC│
│  agent/3ds    : rede, FS(sdmc), UI, config, info do aparelho     │
│  agent/host   : build para macOS/Linux (testes sem console)      │
│  agent/dsi, agent/switch : reservados                            │
└──────────────────────────────────────────────────────────────────┘
```

### Tudo local, sem servidor (decisão)
O Bridge roda **na mesma máquina/LAN do console** (Mac, Windows ou Linux). Não há servidor hospedado, túnel nem exposição na internet: o 3DS só fala TCP+HMAC na LAN com o Bridge, e os clientes MCP (Codex, Claude Code) falam com o Bridge localmente. Isso reduz a superfície de ataque (nenhuma porta aberta para fora, nenhum TLS a portar) e a complexidade. Consequência aceita: clientes que exigem MCP remoto (ChatGPT/Claude web) **não** conseguem usar o Bridge. Se um dia isso for necessário, o núcleo continua reutilizável (ver §15), mas está fora do escopo.

### App desktop (ideia "FileZilla + servidor MCP")
O app é o próprio Bridge com interface: um **daemon local + UI** que conecta ao 3DS e, ao mesmo tempo, **é o servidor MCP**. Vantagens: navegador de arquivos do SD com drag-and-drop, **log de auditoria em tempo real do que a IA está fazendo**, botão de trocar modo (READ ONLY/DEVELOPMENT), aprovação humana de escritas, pairing guiado. Notas: (a) MCP stdio exige que o cliente lance o processo; um app já aberto expõe MCP por **HTTP em 127.0.0.1** (Streamable HTTP, apenas loopback) e/ou oferece `ndev mcp-stdio`, um pequeno repassador para o app em execução; (b) empacotamento: começar com "daemon Node + UI no navegador em localhost" e só depois embrulhar em Tauri/Electron (Mac e Windows); (c) **não é MVP** — entra depois do marco M6. Até lá, o Bridge é só CLI + servidor MCP stdio.

## 3. Componentes

### 3.1 Agent (no console)
Faz só: rede, FS, info do aparelho, responder comandos, aplicar política local, mostrar status. **Uma conexão ativa, uma operação por vez.**

- **`agent/common` (C99, sem STL, sem malloc no caminho quente):** parser/encoder de frames, TLV, dispatcher de comandos, normalização e política de paths, SHA-256/HMAC, máquina de estados de transferência com **orçamento por chamada** (`ndp_step(budget)` não-bloqueante). Buffers estáticos. Isso serve 3DS, DSi (4 MiB de RAM, devkitARM/libnds/dswifi) e Switch (libnx) sem reescrever.
- **`agent/3ds`:** implementa a interface de plataforma: `net_*` (soc:u, sockets não-bloqueantes+`poll`), `fs_*` (POSIX sobre `sdmc:`), `sysinfo_*`, `ui_*` (console libctru — sem ImGui), `config_*`, `time`. Thread de rede separada do main loop de UI/HID (padrão do ftpd), com ring buffer de log/atividade protegido por mutex.
- **`agent/host`:** mesma `common` compilada no Mac com backends POSIX. Serve para: testes unitários do protocolo e política; agente falso para o Bridge; CI sem hardware. (Equivalente ao alvo Linux do ftpd.)
- Tela superior: status/IP/porta/modo/versões; inferior: atividade recente. START = sair, X = alternar modo, Y = abrir/fechar a janela de pareamento, SELECT×2 = esquecer todos os pareamentos. UI simples primeiro.

### 3.2 Bridge (no computador)
Monorepo TypeScript:

| Pacote | Função |
|---|---|
| `core` | Codec NDP, transporte TCP (timeouts, reconexão), `NintendoDevice`, política, auditoria, hashing |
| `platform-3ds` | Paths conhecidos (`/3ds`, dumps do Luma…), parser de crash dump, mapeamento de modelo |
| `mcp` | Servidor MCP (stdio + HTTP) com as tools |
| `cli` | `ndev ping|info|ls|cat|put|pair` — também é a ferramenta de teste manual |
| `apps/desktop` | Fase futura (UI + daemon local) |

```ts
interface NintendoDevice {
  hello(): Promise<DeviceHello>;
  ping(): Promise<{ rttMs: number }>;
  getInfo(): Promise<DeviceInfo>;
  list(path: string): Promise<FsEntry[]>;
  stat(path: string): Promise<FsStat>;
  read(path: string, opts: { offset?: number; length?: number }): AsyncIterable<Uint8Array>;
  write(path: string, data: AsyncIterable<Uint8Array>, opts: WriteOpts): Promise<WriteResult>;
}
// ThreeDSDevice hoje; DsiDevice / SwitchDevice depois. MCP só conhece NintendoDevice.
```

## 4. Protocolo NDP v1 (Nintendo Dev Protocol)

**Decisão:** TCP com **frames binários** (cabeçalho fixo + payload TLV/binário). Não HTTP, não JSON no console.

Justificativa: HTTP exigiria parser de headers/chunking no device; JSON exigiria parser+alocação (ruim no DSi); frames com tamanho conhecido permitem streaming sem alocar o arquivo inteiro, e o Bridge traduz para JSON só do lado do computador. Depuração: o Bridge tem `--trace` que imprime cada frame decodificado; opcionalmente o agent tem modo `debug-text`.

### 4.1 Frame (little-endian)
```
off  size  campo
 0    4    magic        "NDP1" (0x3150444E)
 4    1    version      1
 5    1    kind         1=REQ 2=RES 3=DATA 4=END 5=ERR 6=EVT
 6    2    flags        bit0=MAC presente, bit1=MORE
 8    4    request_id
12    2    command
14    2    status       0 em REQ; código em RES/ERR
16    4    payload_len  ≤ max_frame (padrão 65 536; negociado no HELLO)
20    …    payload
…    16    mac          HMAC-SHA256 truncado (se flags.bit0)
```
Payload de REQ/RES = sequência de campos `[tag u16][len u16][valor]` (inteiros LE; strings UTF-8 sem NUL). Payload de DATA = bytes crus. Campos desconhecidos são ignorados (extensível sem quebrar versão).

Mac = HMAC-SHA256(chave_sessão, `contador_de_frame(u64) ‖ cabeçalho ‖ payload`)[0:16]; contador monotônico por direção impede replay.

### 4.2 Handshake, versão e pairing
1. Bridge → `HELLO {protocol_min=1, protocol_max=1, bridge_name, client_nonce[16]}`
2. Agent → `HELLO {protocol=1, platform="3ds", agent_version, device_nonce[16], auth="required|paired|none", mode, max_frame}`
3. Sem versão em comum → `ERR UNSUPPORTED_PROTOCOL` e fecha (nunca quebra silenciosamente).
4. **Pairing** (primeira vez; implementado na v0.5.0, especificação exata em [`docs/protocol/ndp-v1.md`](docs/protocol/ndp-v1.md) §4): a pessoa aperta **Y** no 3DS → o console gera 80 bits com `PS_GenerateRandomBytes` (sem fallback fraco: sem RNG seguro, não pareia) e mostra `XXXX-XXXX-XXXX-XXXX` **só na tela superior** (nunca no log, que o protocolo consegue ler) por 120 s; a pessoa digita o código no Bridge (`ndev pair <ip>`, exige terminal interativo). O código **nunca trafega**: o Bridge deriva `PSK = SHA-256("NDP-PSK-v1" ‖ código)` e envia só uma prova HMAC (`PAIR`). Cada abertura da janela pareia **um** computador (até 4) e fecha; 5 provas erradas fecham a janela e a conexão.
5. Sessão: `AUTH {key_id, proof}` prova a posse da PSK; `chave_sessão = HMAC(PSK, "session" ‖ cn ‖ dn)`; depois do AUTH **todo frame nas duas direções é selado** (HMAC-16 com contador por direção: sem MAC, MAC errado, adulteração ou replay ⇒ a conexão cai sem resposta). Antes do AUTH só HELLO/PAIR/AUTH são aceitos.

Onde ficam os segredos: no console, `/3ds/nintendo-dev-agent/config/pairing.bin` (com checksum; pasta que **nenhum** comando do protocolo lê ou escreve); no computador, `~/.config/nintendo-dev/keys.json` (modo 0600, ou `$NDEV_KEYS_FILE`), indexado pelo `device_id` do console (o IP muda por DHCP). **SELECT duas vezes** no console apaga todos os pareamentos; `ndev unpair` apaga o do computador. O servidor MCP **não expõe nenhuma ferramenta de pareamento** ao modelo.

Limites honestos: o canal é **autenticado e íntegro, mas não criptografado** (o conteúdo dos arquivos é visível na LAN; TLS exigiria portar mbedtls). Um agente de IA com shell **no mesmo usuário do computador** consegue ler `keys.json` — o pareamento protege contra outros aparelhos da rede e contra quem só tem acesso à LAN, não contra software que já roda como você (esse já tem o MCP). O que ele continua sem poder fazer é mudar as pastas liberadas ou o modo: isso só pelo console. Alternativa futura: ChaCha20-Poly1305 sobre a chave de sessão.

### 4.3 Comandos (IDs e semântica)
| ID | Nome | Modo mínimo | Notas |
|---|---|---|---|
| 0x0001 | HELLO | — | acima |
| 0x0002 | PING | — | eco de `nonce`; mede RTT |
| 0x0004 | PAIR | — | só com a janela de pareamento aberta (Y no console) |
| 0x0005 | AUTH | — | prova de posse; depois dele todos os frames são selados |
| 0x0010 | DEVICE_INFO | READ_ONLY | ver 4.4 |
| 0x0020 | FS_LIST | READ_ONLY | paginado (`cursor`, ≤N entradas por RES) |
| 0x0021 | FS_STAT | READ_ONLY | |
| 0x0022 | FS_READ | READ_ONLY | `offset,length,chunk`; responde RES + N×DATA + END |
| 0x0030 | FS_WRITE | DEVELOPMENT | REQ com metadados + DATA… + END; commit atômico |
| 0x0031 | FS_MKDIR / 0x0032 FS_RENAME | DEVELOPMENT | (pós-MVP) |
| — | FS_DELETE | — | **não implementar** no MVP; futuro: mover para lixeira |
| 0x0040+ | LOG_*, CRASH_* | READ_ONLY | fase 3 |

`FULL` reservado, não implementado.

### 4.4 `DEVICE_INFO` (só o que a API fornece)
`platform`, `model` (`CFGU_GetSystemModel`), `agent_version`, `protocol_version`, `ip` (`gethostid`), `mem_free_app` (`osGetMemRegionFree`), `sd_total`/`sd_free` (`FSUSER_GetSdmcArchiveResource`: `clusterSize × clusters`), `mode`, `device_name`, `system_version` (`osGetSystemVersionData`, opcional). Campo ausente = "não disponível", nunca preenchido com valor inventado.

### 4.5 Exemplos (representação lógica; no fio é binário)
```
→ HELLO    {protocol_min:1, protocol_max:1, bridge_name:"ndev/0.1", client_nonce:<16B>}
← HELLO    {protocol:1, platform:"3ds", agent_version:"0.1.0", device_nonce:<16B>,
            auth:"paired", mode:"DEVELOPMENT", max_frame:65536}
→ PING     {id:7, nonce:0xCAFE}                 ← RES {id:7, status:0, nonce:0xCAFE}
→ DEVICE_INFO {id:8}
← RES {id:8, platform:"3ds", model:"new3ds", agent_version:"0.1.0", protocol_version:1,
       ip:"192.168.1.123", mem_free_app:…, sd_total:…, sd_free:…}
→ FS_LIST  {id:9, path:"/3ds", cursor:0}
← RES {id:9, entries:[{name:"nintendo-dev-agent", type:"dir", size:0}, …], next:0}
→ FS_READ  {id:10, path:"/3ds/nintendo-dev-agent/test.txt", offset:0, length:1048576, chunk:32768}
← RES {id:10, total_size:24, will_send:24}
← DATA {id:10, <24 bytes "Hello from Nintendo 3DS\n">}   ← END {id:10, sha256:<32B>}
→ FS_WRITE {id:11, path:"/3ds/nintendo-dev-agent/from-codex.txt", size:16,
            overwrite:"never", sha256:<32B>}
→ DATA {id:11, <16 bytes>}  → END {id:11}
← RES {id:11, status:0, bytes:16, sha256_ok:true, replaced:false}
```
Erros: `OK, UNSUPPORTED_PROTOCOL, UNAUTHORIZED, FORBIDDEN_MODE, PROTECTED_PATH, NOT_FOUND, EXISTS, IO_ERROR, NO_SPACE, BAD_REQUEST, BUSY, TOO_LARGE, HASH_MISMATCH, TIMEOUT, UNSUPPORTED_COMMAND` + campos `detail` (texto) e `os_result` (ex.: `Result` do libctru) para depuração.

### 4.6 Escrita segura (`FS_WRITE`)
Abre `<destino>.ndp-<id>.tmp` no mesmo diretório → grava por chunk → `fflush`+`fsync` (se disponível) → confere tamanho e SHA-256 (se enviado) → se destino existe: `overwrite=never` ⇒ `EXISTS` (apaga o temp); `overwrite=replace` ⇒ (opcional `backup=true`: renomeia o antigo para `.bak`) → `rename` temp→destino. Falha em qualquer etapa remove o temp. Após desconexão no meio, o agent apaga temps órfãos ao aceitar a próxima conexão. Flow control: TCP + ACK a cada N chunks configurável (medir no hardware).

### 4.7 Paths e proteção
Paths absolutos, `/`, UTF-8, relativos à raiz do SD; a normalização rejeita `..`, `\`, NUL, `//` e comprimento excessivo. Comparação **case-insensitive** (FAT). A política é aplicada **no device (autoritativa) e no Bridge (falha rápida)**. **Modelo: allowlists escolhidas pelo usuário (nega por padrão).**
- `read_roots` (padrão: SD inteiro) e `write_roots` (padrão: **apenas** `/3ds/nintendo-dev-agent/`). O usuário escolhe livremente quaisquer pastas (`/roms`, `/3ds/tmc3ds`, `/cias`…); a UI oferece presets, mas o projeto não impõe nomes nem estrutura.
- A política é **guardada e aplicada no console**. A UI do Bridge pode *pedir* uma mudança, mas o console mostra uma **confirmação física** ("Permitir escrita em /roms? A/B"). Motivo: Codex/Claude Code têm shell na mesma máquina e poderiam chamar a API local da UI; um canal exclusivamente humano impede a IA de ampliar as próprias permissões. O arquivo de política e o de chaves não são graváveis remotamente.
- **Pastas de alto risco** (`/Nintendo 3DS/`, `/luma/`, `/boot.firm`, `/gm9/`, `/private/`): fora do alcance mesmo que um root maior as contenha; liberar exige confirmação extra no console com aviso explícito.
- Com `READ_ONLY` nenhuma escrita é aceita, em nenhuma pasta.
- **Somente SD.** O agent monta apenas `sdmc:`. NAND (CTR/TWL NAND, saves de sistema) está **fora do escopo**: escrever pode brickar e ler expõe dados únicos do console a um LLM. Se um dia for desejado: raiz separada, somente leitura, modo `FULL` + confirmação no console.

## 5. Segurança
- **Build de desenvolvimento (decisão do usuário, 2026-09-24):** enquanto o projeto não for distribuído, o agente abre em `DEVELOPMENT` (escrita só em `/3ds/nintendo-dev-agent`). Desde a v0.5.0 o **pareamento é obrigatório** (`AGENT_AUTH_REQUIRED=1`), então só computadores pareados escrevem. **Antes de qualquer distribuição:** trocar `AGENT_START_MODE` para `NDP_MODE_READ_ONLY` (`agent/3ds/source/main.c`).
- Modos: `READ_ONLY` (padrão; nenhuma escrita) → `DEVELOPMENT` (escrita só em `write_roots`) → `FULL` (reservado). Trocar modo só pelo console (botão) ou config no SD — nunca por comando remoto.
- Nenhum comando (além de HELLO/PAIR/AUTH) sem AUTH; limite de 1 conexão; timeouts em todo estágio; 5 falhas de AUTH/PAIR derrubam a conexão (e fecham a janela de pareamento).
- **UI/API local:** escuta só em `127.0.0.1`; valida `Host` e `Origin` (defesa contra DNS rebinding e páginas maliciosas que chamem `localhost`); token por sessão; sem CORS aberto. Acesso de outros dispositivos da LAN (ex.: celular) só como opt-in explícito, com token.
- Prompt injection: logs, dumps e arquivos do SD entram no contexto do LLM como **dados**; as descrições das tools dizem isso, e o Bridge marca saídas de arquivo como conteúdo não confiável.
- Toda ação vai para o **audit log** do Bridge (quem, o quê, path, resultado, hash).
- Tool annotations do MCP (`readOnlyHint`/`destructiveHint`) são úteis mas **não confiáveis**; a política real é a do Bridge/agent.
- Instalação de CIA / apagar títulos: fora do escopo até estudo próprio; sempre com confirmação humana no console.

## 6. Tools MCP (design)

Nomes com underscore (`nintendo_fs_read`). Descrições explicitam: hardware real; `write` modifica o SD; caminhos sensíveis; falhas possíveis.

| Tool | Notas de design |
|---|---|
| `nintendo_ping`, `nintendo_device_info` | `structuredContent` + texto |
| `nintendo_fs_list`, `nintendo_fs_stat` | paginação |
| `nintendo_fs_read` | **limitada**: `offset`, `length`, `max_bytes` (padrão baixo); `encoding: utf8|hex|base64`; retorna truncamento explícito. Não devolve arquivos grandes ao LLM |
| `nintendo_fs_download` (fase 2) | copia remoto → **caminho local** no Mac e devolve só metadados + hash; o agente lê o arquivo localmente |
| `nintendo_fs_write` | conteúdo pequeno inline, `overwrite: never|replace`, `backup` |
| `nintendo_fs_upload` (fase 2) | caminho **local** → remoto (binários) |

O chunking do fio (32–64 KiB) é independente do que o LLM enxerga.

**Versão MCP (verificado):** a spec mais recente é **2026-07-28**, mas Claude Code 2.1.177 e Codex 0.134.0 ainda abrem com `initialize` (2025-11-25 e 2025-06-18). **Decisão:** `@modelcontextprotocol/server` 2.1.0 com `serveStdio(factory)`, que atende as duas eras (evidência em `tests/mcp-hello`). Em HTTP local (app desktop) a mesma compatibilidade dual precisa ser testada no marco M9.

## 7. Estrutura do repositório

```
nintendo-dev-mcp/
├─ ARCHITECTURE.md
├─ README.md   LICENSE (Apache-2.0)   NOTICE   CHANGELOG.md
├─ docs/
│  ├─ research/findings.md          # Etapa A
│  ├─ protocol/ndp-v1.md            # spec canônica + tabela de erros
│  ├─ protocol/test-vectors/*.json  # frames de exemplo (usados por C e TS)
│  ├─ security.md   roadmap.md   hardware-test-checklist.md
├─ agent/
│  ├─ common/   include/ndp/*.h  src/*.c     # C99, sem dependência de SO
│  ├─ 3ds/      Makefile (devkitPro)  source/ meta/(icon, smdh)
│  ├─ host/     CMake  (macOS/Linux)  — agente falso + testes
│  ├─ dsi/  switch/                          # README de intenção apenas
├─ bridge/
│  ├─ packages/{core,platform-3ds,mcp,cli}/
│  └─ apps/desktop/                          # futuro
├─ tests/
│  ├─ integration/   # bridge ⇄ agent/host
│  └─ hardware/      # roteiros manuais + scripts de diagnóstico
└─ .github/workflows/ (ci.yml, release.yml)
```

## 8. Dependências
- **3DS:** devkitARM r68, libctru 2.7.0, `3dstools` (smdhtool/3dsxtool) — **já instalados**. Sem citro2d/ImGui/zlib/mbedtls no MVP. SHA-256/HMAC próprios (domínio público). Build: Makefile clássico do devkitPro (mais simples) ou CMake com `3DS.cmake` (presente). `makerom` só para CIA/CXI (fase futura).
- **Computador:** Node ≥ 22 (temos v26), TypeScript, SDK MCP (ver §6), `zod`, runner de testes (vitest/`node:test`). Opcional: `arm-none-eabi-addr2line` (já no devkitARM) para crashes.
- **Host agent (testes):** CMake + clang do macOS.

## 9. Decisões aproveitadas do ftpd × o que **não** copiar
Aproveitar (conhecimento): ordem de init de rede do 3DS; sockets não-bloqueantes+`poll`; recuperação "rede perdida → derruba tudo → re-listen"; thread de rede separada; log limitado em RAM; alvo de host para testes; não usar mtime lento por padrão.
Não copiar: FTP; C++20/STL/GSL/ImGui/curl/jansson; credencial em texto; escrita direta no destino; código GPL-3.0 (ver licença em `findings.md`); mDNS próprio.

## 10. Discovery
MVP: IP manual. Para uso por outras pessoas, a descoberta automática **sobe de prioridade** (entra antes do primeiro release público). Formato: **beacon UDP** (Bridge faz broadcast de "NDP-DISCOVER", agent responde com nome/modelo/porta) — muito mais simples que mDNS no libctru (o mDNS do ftpd tem ~600 linhas). Sem descoberta automática no MVP.

## 11. Background (residente) — visão de roadmap
**Investigado em 2026-09-25 ([`docs/research/background-agent.md`](docs/research/background-agent.md)):** o Luma3DS só *substitui* módulos de sistema existentes; não inicia um título novo no boot, e o modo sysmodule do sys-ftpd é não comprovado (CXI "pending"). Um agente sempre ligado exigiria sequestrar o slot de um módulo existente — **não recomendado**. Para o loop de desenvolvimento e o diagnóstico há alternativas sem sysmodule: **GDB do Rosalina** (crash/backtrace pela rede), **`3dslink`** (enviar+executar+`stdout` ao Mac) e o agente atual em primeiro plano para arquivos. Um interruptor no app só faria sentido se surgir um CXI viável.
## 12. Crashes/logs (fase 3)
Luma grava `crash_dump_NNNNNNNN.dmp` em `…/dumps/arm11/` **somente se o usuário apertar A** na tela de exceção; a pasta exata (`/dumps` vs `/luma/dumps`) deve ser confirmada no hardware. Formato conhecido (`ExceptionDumpHeader` + registradores/código/pilha/proc). O Bridge terá parser próprio e usará `addr2line` com o `.elf`/`.map` local; precisamos do endereço-base do 3DSX (a definir). Tools: `logs.list/read`, `crashes.list/latest/read`.

## 13. CI/CD
GitHub Actions: (1) testes do Bridge + host agent; (2) build do agente 3DS (imagem `devkitpro/devkitarm`) gerando `.3dsx` + `.smdh`; (3) release com `.3dsx`, Bridge, `SHA256SUMS`, changelog. CIA avaliado separadamente. O remoto GitHub será criado por você (o token local não cria repositórios).

### Distribuição para outras pessoas
- **Licença:** como não copiamos código GPL do ftpd, podemos escolher **Apache-2.0** (decidido; titular `BC Estratégias` em `NOTICE` — ajustar se necessário). Precisa estar definida **antes** de qualquer release público. Dependências de terceiros e a atribuição delas entram em `THIRD_PARTY_LICENSES`.
- **Artefatos por release:** `nintendo-dev-agent.3dsx` (+ `.smdh`) para o console; Bridge/app desktop para macOS (Apple Silicon e Intel), Windows e Linux; `SHA256SUMS`; changelog. Assinatura/notarização no macOS e no Windows é trabalho à parte (custo e conta de desenvolvedor) — sem isso o usuário verá avisos do sistema. CIA fica para depois.
- **Nada específico da sua máquina:** sem IPs, caminhos ou nomes fixos no código; configuração por usuário (`~/.config/nintendo-dev/` ou equivalente), auditoria e chaves de pairing locais.
- **Seguro por padrão:** primeira execução em `READ_ONLY`; pairing obrigatório; caminhos protegidos ativos. Outras pessoas terão saves e CFW próprios — o custo de um bug destrutivo sobe. Isso torna a escrita atômica, os backups e o `fs.delete` só via lixeira requisitos, não extras.
- **Compatibilidade entre versões:** o handshake com `protocol_version` e mensagem clara de "atualize o agent/Bridge" passa a ser requisito de produto, pois agent e Bridge terão ciclos de atualização independentes.
- **Ambientes variados:** Windows precisa entrar no CI cedo (paths, firewall na primeira escuta, terminador de linha); redes com isolamento de clientes (AP isolation) e redes 2.4 GHz-only devem estar na documentação de solução de problemas.
- **Documentação de usuário:** guia "instalar em 5 minutos" (copiar `.3dsx`, instalar o app, parear), checklist de diagnóstico, aviso claro de que exige CFW (Luma3DS) e que o uso é por conta e risco do usuário. Não distribuímos firmware, chaves nem conteúdo da Nintendo.
- **Sem telemetria.**

## 14. Roadmap e marcos (cada marco termina com um teste **no seu New 3DS**)

| Marco | Entrega | Prova em hardware |
|---|---|---|
| **M0** ✔ | Repo, docs, protocolo escrito ([`docs/protocol/ndp-v1.md`](docs/protocol/ndp-v1.md)), vetores independentes (Python), codec TS + `common` C com testes, host agent, CLI `ndev` | nenhuma (tudo no Mac): `scripts/check.sh` |
| **M1** ✔ | Agent 3DS: Wi-Fi, IP, porta, HELLO/PING; `ndev ping` | validado no New 3DS (RTT ~4 ms) |
| **M2** *(D)* | `DEVICE_INFO` | `ndev info` mostra modelo, IP, SD, memória |
| **M3** ✔ | `FS_LIST`, `FS_STAT` | validado no New 3DS (`ndev ls`) |
| **M4** ✔ | `FS_READ` em streaming | `cat test.txt` = "Hello from Nintendo 3DS" no 3DS real; ~1 MiB/s |
| **M5** ◐ | `FS_WRITE` atômico + `FS_MKDIR` + modos + proteção de paths ✔ (v0.3.0, testes no Mac); **pareamento/HMAC ✔ (v0.5.0, testes no Mac; a validar no console)** | `from-codex.txt` no SD real (validado sem auth na v0.4.0) |
| **M6** ✔ | Servidor MCP (stdio) com 12 ferramentas, descoberta automática, auditoria e sandbox local ([`docs/mcp.md`](docs/mcp.md)) | 13 testes ponta a ponta no Mac; **pendente:** teste com modelo real e com o console real |
| M7 | `fs_upload/download`, `deploy_homebrew` (hash, backup, temp→rename; estudar `3dslink` antes) | build `.3dsx` do TMC3DS enviado e substituído |
| M8 | Logs e crashes (+ parser Luma) | "analise o último crash" |
| M9 | App desktop (UI + MCP em 127.0.0.1) | UI mostra a IA agindo |
| M10+ | Agente residente; CIA install (com confirmação); benchmark; DSi; Switch | — |

Regra de depuração (do prompt): erro → identificar a camada → adicionar diagnóstico → hipótese → testar → corrigir; sem mudanças aleatórias. "Concluído" só quando validado no hardware, não porque compila.

## 15. Riscos e questões em aberto
1. Teste ponta a ponta (modelo chamando a tool) nos clientes reais; compatibilidade dual também em HTTP local.
2. Pasta exata dos dumps do Luma; endereço-base para `addr2line`.
3. Chunk ideal (16 vs 64 KiB) e throughput real — medir.
4. Custo de SHA-256 no ARM11 — medir; fallback: Bridge relê e compara.
5. Emulador (Azahar/Citra) rodando o agente com sockets do host — testar; reduziria muito o ciclo.
6. Modelo de chave: PSK em texto no SD; canal sem criptografia — decidir se aceitável no MVP.
8. Agente residente: efeito do NDM exclusivo em jogos e uso real de memória.
9. Nome público do projeto (evitar marcas da Nintendo no nome/ícone distribuídos).
10. Assinatura/notarização dos apps desktop (macOS/Windows) e custo associado.
11. MCP remoto (ChatGPT/Claude web) está **fora do escopo** por decisão; se voltar a ser desejado, exigiria um túnel de saída a partir do Bridge local e reavaliação de segurança.
