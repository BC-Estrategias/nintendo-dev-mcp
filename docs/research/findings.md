# Etapa A — Pesquisa (achados verificados)

Data: 2026-09-24. Cada item indica como foi verificado:
**[código]** lido no fonte clonado · **[header]** lido no header local do libctru · **[web]** lido em documentação online · **[não verificado]** hipótese, exige teste.

## 0. Versões encontradas

| Item | Versão | Fonte |
|---|---|---|
| devkitARM | r68-1 (gcc 16.1.0) | pacman local |
| libctru | 2.7.0-1 | pacman local |
| citro2d/citro3d | 1.7.0 / 1.7.1 | pacman local |
| `3ds-zlib`, mbedtls etc. em `portlibs/3ds` | **não instalados** (`portlibs/3ds/lib` vazio) | ls local |
| ftpd (mtheall) | v3.2.1-2, último commit 2025-11-18 | git clone |
| sys-ftpd (themoonisacheese) | fork do ftpd com modo sysmodule 3DS (`-DFTPD_SYSMODULE=ON`) | git clone |
| Luma3DS | master de 2026-09-02 | git clone |
| Spec MCP mais recente | **2026-07-28** | modelcontextprotocol.io |
| SDK MCP TypeScript | `@modelcontextprotocol/server` **2.1.0** (spec 2026-07-28); `@modelcontextprotocol/sdk` 1.30.1 (linha v1) | npm view |
| Clientes locais | Codex CLI 0.134.0, Claude Code 2.1.177 | `--version` |

Correção de premissas iniciais (minhas, não suas):
- `sys-ftpd` **é** um sysmodule de 3DS (fork do ftpd). Eu havia dito que era da Switch — errado para este repositório.
- ftpd é **GPL-3.0**, não MIT (arquivo `LICENSE`, cabeçalhos dos fontes).

## 1. ftpd — como está estruturado

Um único core em C++20 (~17 mil linhas, `-Wall -Wextra -Werror`) compilado para 3DS, Switch, NDS e Linux. Dependências: GSL (baixada no configure), Dear ImGui 1.91.8 (idem, com `imgui.patch`), zlib, e libcurl/jansson só nas builds gráficas.

| Arquivo | Responsabilidade |
|---|---|
| `main.cpp` (77 linhas) | `platform::init()` → `FtpServer::create()` → loop `platform::loop()/draw()/render()` → `platform::exit()` |
| `include/platform.h` | **Fronteira de plataforma**: `init/loop/render/exit`, `networkVisible`, `networkAddress`, `hostname`, `Thread`, `Mutex`, `steady_clock` |
| `source/{3ds,switch,nds,linux}/platform.cpp` | Implementação por alvo (3DS: 849 linhas) |
| `ftpServer.cpp` | Dono do socket de escuta, lista de sessões, thread de rede, eventos de rede achada/perdida |
| `ftpSession.cpp` (3523 linhas) | Máquina de estados do protocolo (COMMAND → DATA_CONNECT → DATA_TRANSFER), buffers, handlers |
| `socket.cpp/h`, `sockAddr.*` | Wrappers RAII sobre sockets POSIX |
| `ioBuffer.*` | Buffer `[inutilizável][usado][livre]` com `coalesce()` |
| `fs.cpp/h` | Wrappers RAII sobre `FILE*`/`DIR*` (`setvbuf` com buffer próprio) |
| `ftpConfig.*` | Config `key=value` em texto |
| `mdns.cpp` (599 linhas) | mDNS próprio (RFC 1035/6762), exceto NDS |
| `source/3ds/imgui_*` | UI (ImGui sobre citro3d) |

Não há suíte de testes (o próprio `AGENTS.md` do repo diz isso).

### Respostas às perguntas do prompt (itens 1–11)

**1. Código compartilhado vs. específico do 3DS.** Compartilhado: `FtpServer`, `FtpSession`, `Socket`, `SockAddr`, `IOBuffer`, `fs`, `FtpConfig`, `log`, `mdns`. Específico do 3DS: `source/3ds/*` (init de serviços, backlight, ganchos APT, UI, relógio via ticks do ARM11) e uns `#ifdef __3DS__` pontuais em `ftpSession.cpp` (mtime via serviço FS, owner/group fictícios "3DS", offset de fuso). **[código]**

**2. Init de rede no 3DS.** **[código: `source/3ds/platform.cpp:61-258`]**
- `acInit()` + `ndmuInit()` + `ptmuInit()` na `platform::init()`.
- O loop chama `startNetwork()` **a cada iteração** até dar certo. Ela: consulta `ACU_GetWifiStatus()`; se conectado, aloca o buffer do `soc:u` com `memalign(0x1000, 0x100000)` (1 MiB) e chama `socInit(buf, 0x100000)`.
- Depois `aptSetSleepAllowed(false)` e `NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE)` + `NDMU_LockState()` para o sistema não derrubar o Wi-Fi.
- IP via `gethostid()`; se voltar `INADDR_BROADCAST` trata como "sem IP".
- `osSetSpeedupEnable(true)` (New 3DS).

**3. Sockets.** `Socket` = RAII sobre `fd`. Opções: `SO_REUSEADDR`, `SO_LINGER`, `SO_RCVBUF/SO_SNDBUF`, não-bloqueante via `ioctl(FIONBIO)` (3DS/NDS) ou `fcntl`. Leitura/escrita via `recv/send` logando erro **exceto** `EWOULDBLOCK`. `listen(10)`. **[código: `socket.cpp`, `ftpServer.cpp:437-484`]**

**4. Filesystem.** Sem camada VFS própria: newlib/POSIX (`fopen`, `opendir`, `stat`, `rename`) sobre o devoptab `sdmc:` do libctru. Única exceção: mtime real via serviço FS (`archive_getmtime`), que o projeto trata como **lento** e deixa opcional (`mtime=0/1`). `lstat` é `#define`d para `stat` (sem symlinks). **[código]**

**5/6. Leitura/escrita e transferências grandes.** Estados por sessão; a cada evento de `poll()` roda até **10 iterações** do handler de transferência. Buffers: 64 KiB de transferência, 32 KiB de socket no 3DS, `FILE` com `setvbuf` de 4×64 KiB = 256 KiB. `retrieveTransfer`: lê um bloco do arquivo para o `IOBuffer`, `send()` não-bloqueante; se `EWOULDBLOCK`, retorna e espera o próximo `poll`. `storeTransfer`: `recv()` → `fwrite()`. **Nunca carrega o arquivo inteiro.** Escreve **direto no destino** — sem arquivo temporário, sem rename, sem verificação; upload interrompido deixa arquivo truncado (a nossa proteção "temp → validar → rename" é trabalho novo). **[código: `ftpSession.cpp:2216-2371`]**

**7. Erros de socket.** `POLLERR|POLLHUP` no socket de dados → responde `426` e volta a COMMAND; no de comando → `closeCommand()`. Falha em `accept()` ou `poll()<0` no servidor → `handleNetworkLost()`: destrói **todas** as sessões e sockets de escuta; o loop então reavalia o Wi-Fi e re-listen. **[código: `ftpSession.cpp:665-764`, `ftpServer.cpp:486-504, 1009-1069`]**

**8. Desconexões.** Timeout ocioso de 60 s (`IDLE_TIMEOUT`) fecha a sessão. Sessão marcada `dead()` é removida a cada volta do loop. **[código]**

**9. Config.** Texto `key=value` em `/config/ftpd/ftpd.cfg` (parser manual, `mkdirParent`). Guarda `user` e `pass` **em texto puro**. Observação **[código, não testado em hardware]**: em `FtpConfig::save`, a linha `deflateLevel=%u` é impressa sem `\n`; na build 3DS a linha seguinte `mtime=%u` sai colada (`deflateLevel=6mtime=1`), e o parser rejeitaria o valor. Isto é um bug provável, não uma certeza.

**10. Main loop / não bloquear.** O servidor roda numa **thread própria** (3DS: `threadCreate` com prioridade = atual+1, pilha 0x8000, appcore). O main thread faz `aptMainLoop()` → `startNetwork()` → `hidScanInput()` → UI. A thread de rede usa `poll()` com 0 ms no listen, 100 ms nas sessões e `sleep(16 ms)` quando ocioso. Só o NDS é single-thread (`LOCKED(x)` vira no-op). Estado compartilhado protegido por `platform::Mutex`. **[código]**

**11. Como várias plataformas compartilham o core.** `platform.h` + um `platform.cpp` por alvo + matriz de `#ifdef` (`CLASSIC`, `SYSMODULE`, `NO_IPV6`, `__3DS__`…). Tamanhos de buffer variam por alvo (NDS 4–8 KiB, 3DS 32 KiB, demais 64 KiB). Existe alvo **Linux** para desenvolver/depurar sem console — ideia que vamos copiar (ver §"Decisões" no ARCHITECTURE.md).

### O que aproveitamos como *conhecimento* (sem copiar código)
1. Sequência `acInit → ACU_GetWifiStatus → memalign 1 MiB → socInit → NDM exclusive+lock → aptSetSleepAllowed(false)`.
2. Polling contínuo do status do Wi-Fi e recuperação por "rede perdida → derruba tudo → re-listen".
3. Sockets não-bloqueantes + `poll()` + tratamento explícito de `EWOULDBLOCK`.
4. Thread de rede separada do main loop de UI/HID, com estado compartilhado via mutex.
5. Log limitado em memória no 3DS (ftpd: 250 mensagens; 10 000 nos demais).
6. Não usar `mtime` via serviço FS por padrão (lento).
7. Alvo de host (Linux/macOS) para testar o core sem console.

### O que **não** copiamos
FTP em si; C++20/STL/GSL/ImGui/curl/jansson (pesados demais para DSi e desnecessários); credenciais em texto puro; escrita direta no destino; mDNS próprio (599 linhas — ver discovery); código sob GPL (ver licença).

### Licença
ftpd e sys-ftpd são **GPL-3.0**. Copiar código deles obriga o nosso projeto a ser GPL-3.0-compatível. Decisão: **escrever nossa própria camada**, usando o ftpd só como referência de comportamento/hardware. Nosso código: Apache-2.0 (decidido). libctru é zlib, compatível. Se um dia copiarmos qualquer trecho, isso precisa ser uma decisão explícita e documentada.

## 2. sys-ftpd — como virou serviço residente no 3DS

Verificado no fonte, `meta/ftpd-sys.rsf`, `README.md` e `AGENTS.md` do fork **[código/doc]**:

- **Formato:** CXI (`000401300000D902.cxi`, Title ID `000401300000D902`, categoria *Base*), colocado em `/luma/sysmodules/` e carregado pelo Luma3DS no boot. Exige ativar "Enable loading external FIRMs and modules" no menu do Luma (SELECT no boot). Depende de **Luma3DS**.
- **Build:** mesma base do ftpd com `-DFTPD_SYSMODULE=ON` (força CLASSIC, sem gfx/apt/hid). Precisa de `makerom` para empacotar o CXI (não verifiquei se está instalado aqui).
- **Init customizado:** sobrescreve `__appInit` (só `srvInit`, `fsInit`, `archiveMountSdmc`), `__appExit` e `__system_allocateHeaps`, que pede heap de **0x2000000 (32 MiB)** + linear de **0x1000000 (16 MiB)** da região **BASE** (`MEMOP_REGION_BASE`), sem tocar na região APPLICATION dos jogos.
- **Serviços/permissões** (RSF): `ac:u`, `cfg:u`, `fs:USER`, `ndm:u`, `ptm:u`, `soc:U`, `nwm::UDS`; FS `DirectSdmc` + `DirectSdmcWrite`; `RunnableOnSleep: true` (funciona com a tampa fechada); pilha 0x40000; lista explícita de syscalls permitidas.
- **Comportamento:** headless, sem ícone no Home; inicia o servidor quando o Wi-Fi conecta e derruba quando desconecta; config **somente leitura** em runtime (`/config/ftpd/ftpd.cfg`); thread principal e thread de rede dormem 250 ms quando ociosas para poupar bateria.
- **Riscos documentados pelo próprio projeto:** sysmodule defeituoso pode impedir o boot (remover o `.cxi` pelo PC).

### Implicações para nós (fase futura — não implementar agora)
| Tema | Achado / hipótese |
|---|---|
| Memória | Números acima são o que o sys-ftpd *pede*; consumo real e se o pedido funciona em Old vs New 3DS **precisa ser medido** [não verificado]. |
| Serviços | `soc:U` limita: 64 sockets no total do sistema, 18 sessões soc:U, 32 processos [web: 3dbrew Socket Services]. Um jogo que use rede compete pelos mesmos sockets. |
| NDM | O sys-ftpd ainda chama `NDMU_EnterExclusiveState(INFRASTRUCTURE)`+`LockState` no modo sysmodule. **Hipótese** [não verificado]: isso pode interferir em jogos com Wi-Fi local/StreetPass. Testar. |
| Segurança | Um servidor sempre ligado, com escrita no SD (`DirectSdmcWrite`), sem UI: **pairing precisa de canal alternativo** (ex.: chave colocada no SD por quem tem posse física) e modo padrão READ ONLY. |
| Compatibilidade | Jogos rodando simultaneamente é justamente o caso de uso; requer testes com jogos reais. |
| Requisito | Luma3DS + módulos externos habilitados. |

Conclusão para o roadmap: o **loop de desenvolvimento completo** (deploy → executar → coletar log sem perder a conexão) depende de um agente residente; o app foreground do MVP não basta. Alternativas de menor custo antes disso: o app sob teste loga por socket direto ao Mac (como o `link3dsStdio()` do libctru faz), ou grava log no SD para leitura posterior.

## 3. libctru / toolchain (o que existe hoje) — **[header]**

- Info do aparelho: `CFGU_GetSystemModel()` (enum `CFG_MODEL_3DS/3DSXL/N3DS/2DS/N3DSXL/N2DSXL`), `osGetSystemVersionData()`, `osGetMemRegionSize/Used/Free(region)`, `osGetKernelVersion()`.
- SD: `FSUSER_GetSdmcArchiveResource()` → `sectorSize, clusterSize, totalClusters, freeClusters` (dá espaço livre/total).
- RNG: `PS_GenerateRandomBytes(void*, size_t)`.
- **Não há** API pronta de SHA-256 genérico (só `PS_SignRsaSha256`/`PS_VerifyRsaSha256`, que servem a outro propósito) e mbedtls/zlib **não estão instalados** → SHA-256/HMAC ficam por nossa conta (implementação pequena de domínio público).
- `3dslink.h`: `link3dsConnectToHost(redirStdout, redirStderr)`, porta `17491` — mecanismo existente de stdout/stderr → PC. Ferramenta `3dslink` instalada em `/opt/devkitpro/tools/bin`.
- CMake: `/opt/devkitpro/cmake/3DS.cmake` presente (o ftpd usa CMake; nós podemos usar Makefile devkitPro clássico ou CMake — ver ARCHITECTURE).

## 4. Luma3DS — crashes — **[código do Luma]**

- Local: `dumps/arm<N>/` na raiz do SD (`arm9/source/exceptions.c:186`: `sprintf(folderPath, "dumps/arm%u", processor)`), ou seja `/dumps/arm11/` — os relatos da comunidade citam `/luma/dumps/arm11/` **[web: issues do Luma]**. **Os dois textos não coincidem; a pasta exata deve ser confirmada no hardware** antes de virar constante (o Bridge tratará como lista configurável de candidatos).
- Nome: `crash_dump_%08lu.dmp` com contador incremental (`arm9/source/fs.c:412-425`).
- O dump só é salvo se o usuário **apertar A** na tela de exceção ("Press A to save the crash dump") → não é automático; a UI do Luma precisa estar ativa. **[código]**
- Formato: `ExceptionDumpHeader` = `magic[2]` (`0xDEADC0DE`,`0xDEADCAFE`), versão, `processor` (9/11), `core`, `type`, `totalSize`, `registerDumpSize`, `codeDumpSize`, `stackDumpSize`, `additionalDataSize`; depois registradores (r0–r12, sp, lr, pc, cpsr…, FAR/FSR para data abort), dump de código, pilha e dados adicionais (no ARM11: nome do processo de 8 bytes + Title ID de 8 bytes). **[código: `arm9/source/types.h`, `exceptions.c`]**
- Não existe parser oficial mantido (issue #1990 no Luma diz o script antigo sumiu, e GDB não entende o formato) **[web]** → escreveremos um parser no Bridge (`platforms/3ds/crashes`), com testes usando dumps reais.
- Para mapear PC → fonte com `arm-none-eabi-addr2line` é preciso o **endereço-base** onde o `.3dsx` foi carregado (relocado); a estratégia (subtrair base conhecido, ou registrar base no log do app) é **questão em aberto** [não verificado].

## 5. MCP — o que mudou (spec 2026-07-28) — **[web]**

- **Sem sessão/`initialize`:** requisições autocontidas; versão e capabilities do cliente vão em `_meta` de cada requisição. Compatibilidade com revisões antigas por detecção (há matriz na spec).
- Transportes padrão: **stdio** e **Streamable HTTP** (POST único num endpoint; resposta JSON ou SSE por requisição). Transportes customizados sobre TCP devem reutilizar o framing do stdio.
- Tools: `inputSchema` (JSON Schema 2020-12), `outputSchema` opcional, `structuredContent`, `isError` para erros de execução vs. erros JSON-RPC para protocolo, `annotations` **não confiáveis**. Tipos de conteúdo: text, image, audio, `resource_link`, recurso embutido.
- Extensão **Tasks** (operações longas assíncronas com polling) — candidata para `deploy`/`benchmark`.
- Servidores "devem" validar entradas, aplicar controle de acesso, limitar taxa, sanitizar saídas; clientes "devem" pedir confirmação e ter humano no loop.
- SDK: `@modelcontextprotocol/server` **2.1.0** (novo) e `@modelcontextprotocol/sdk` **1.30.1** (linha anterior).
- **Verificado (2026-09-24, `tests/mcp-hello`):** Claude Code 2.1.177 abre com `initialize` **2025-11-25**; Codex 0.134.0 com `initialize` **2025-06-18**. Nenhum fala 2026-07-28. `serveStdio(factory)` do SDK v2 (`@modelcontextprotocol/server/stdio`) atende as duas eras (padrão `'serve'` para aberturas 2025), e um servidor hello respondeu corretamente a ambos os handshakes em teste roteirizado. **Decisão: usar o SDK v2.** A chamada de tool feita pelo modelo nos clientes reais não foi exercitada (falhas de autenticação/versão do modelo no ambiente de teste).

## 6. Não verificado / pendências
1. Emulador (Azahar/Citra) com sockets de host para rodar o agente sem hardware — pesquisa web não confirmou; testar.
2. `makerom` instalado? (só necessário para CIA/CXI, não para 3dsx).
3. Pasta exata dos dumps do Luma (`/dumps/` vs `/luma/dumps/`).
4. ~~Codex/Claude Code vs. spec MCP 2026-07-28~~ — resolvido: ambos usam a era `initialize`; SDK v2 atende. Falta um teste ponta a ponta com o modelo chamando a tool.
5. Throughput real de SD e Wi-Fi no seu New 3DS (chunk 16 vs 64 KiB).
6. Custo de SHA-256 em software no ARM11 (New 3DS com speedup ligado).
7. Consumo de memória e efeito do NDM exclusivo num agente residente.
