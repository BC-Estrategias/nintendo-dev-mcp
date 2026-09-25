# Agente em segundo plano no 3DS — investigação de viabilidade (2026-09-25)

Pergunta do usuário: dá para o agente rodar "no fundo" via Luma3DS, com um interruptor no app para ligar/desligar?
Método: leitura do código do Luma3DS (master de 2026-09-02, commit `aef3130`), leitura **somente-leitura** do SD do usuário através do próprio agente, 3dbrew e o fork sys-ftpd. Nada foi construído nem instalado.

## Evidências
| Fato | Fonte |
|---|---|
| `enable_external_firm_and_modules = 1` **já está ligado** no Luma do usuário (config v3.13, formato `config.ini`) | `ndev cat /luma/config.ini` |
| `/luma/sysmodules/` já tem `TwlBg.cxi` (módulo customizado) e `.ips` → carregar CXI externo **funciona** neste console | `ndev ls /luma/sysmodules` |
| `plugin_loader_enabled = 1` (plugins 3GX) | `config.ini` |
| O Luma **só substitui** código de módulos **que já existem**: (a) módulos do FIRM, por nome de 8 caracteres (`arm9/source/firm.c:510`, exige `appTitle` igual ao do módulo existente, senão "invalid or corrupted"); (b) código de um sysmodule por ID quando o `pm` o abre (`sysmodules/loader/source/loader.c:171-191`, `patcher.c:341`). Não há no código nada que **inicie um título novo**. | fonte do Luma |
| `000401300000D902` (ID usado pelo sys-ftpd) **não consta** na lista de módulos de sistema; o boot só carrega módulos de uma lista fixa | 3dbrew *Title list* |
| O README do fork sys-ftpd lista o CXI como **"(pending)"** (nunca publicado): o modo sysmodule **não está comprovado** funcionando | `README.md` do fork |
| O Rosalina tem **stub de GDB por TCP** (portas base 4000…4003, "Debugger options → Enable debugger") | `sysmodules/rosalina/source/gdb/server.c`, `menus/debugger.c` |
| Ferramentas: `makerom` **não instalado**; o `pacman` do devkitPro está quebrado neste Mac (erro GPGME) — só afeta instalar pacotes; `makerom` daria para compilar do código-fonte | testes locais |

## Conclusão (com o grau de certeza de cada parte)
1. **Um sysmodule *novo* que sobe sozinho no boot não é suportado pelo Luma pelo que o código mostra.** O que existe é *substituir* um módulo existente. Portanto o modo sysmodule do sys-ftpd provavelmente **não inicia** — mas isso é leitura de código, **não foi testado no console** (o único teste conclusivo é construir o CXI e ver se sobe).
2. O caminho restante para "sempre ligado" seria **sequestrar o slot de um módulo de sistema existente e dispensável** — arriscado (perda de função do sistema, boot loop recuperável só com leitor de cartão) e exige identificar qual módulo é seguro. **Não recomendado agora.**
3. Um interruptor no app ("habilitar/desabilitar") é uma boa ideia de arquitetura **se** existir um CXI que suba: o app copia/remove o `.cxi` em `/luma/sysmodules/` por ação local do usuário (não pelo protocolo — `/luma` continua `never_write` para a rede) e grava o modo numa config protegida. Hoje não há CXI viável para ligar.
4. **Alternativas que resolvem o objetivo (loop de desenvolvimento e diagnóstico) sem sysmodule**:
   - **GDB do Rosalina** para analisar crash/backtrace de homebrew (TMC3DS) pela rede: nenhum código novo no console; `arm-none-eabi-gdb` já vem no devkitARM. *Não testado.*
   - **`3dslink` (netload)**: envia e **executa** o `.3dsx` pelo Wi-Fi com o Homebrew Launcher no modo netload (usuário aperta Y uma vez) e redireciona o `stdout` do app para o Mac (`link3dsStdio`) → o loop "compilar → enviar → rodar → ler log" sem agente residente. *Não testado neste projeto.*
   - **Plugin 3GX** roda dentro de um jogo/app específico, não é "sempre ligado".
   - O agente atual (primeiro plano) continua servindo para operações de arquivo no SD.

## Objetivo real do usuário e caminhos (o projeto é GERAL, não de um jogo)
O que se quer é **acessar logs/arquivos em tempo real com um jogo aberto**, sem sair dele para abrir o agente. O projeto é para **qualquer jogo/homebrew**; o TMC3DS (`/Users/macmini/zelda-tmc-3ds`, projeto do próprio usuário, que já usa rede e grava `tmc3ds.log`) é só **um** exemplo. Três "hospedeiros" do mesmo núcleo de agente (`agent/common` + `agent/posix`):

| Hospedeiro | Serve para | Estado |
|---|---|---|
| **App em primeiro plano** (hoje) | operações de arquivo no SD, deploy | funcionando |
| **Biblioteca embutida** (`libndev`) | homebrew que o desenvolvedor controla (logs ao vivo por frames `EVT`, SD, crash) | não iniciado |
| **Plugin 3GX do Luma**, ligado **por jogo** no front-end | **qualquer jogo**, sem alterá-lo | não iniciado; viabilidade parcial |

### Plugin 3GX por jogo — o que se verificou no código do Luma (não testado no console)
- O loader procura `/luma/plugins/<TitleID de 16 hex>/*.3gx` e cai em `/luma/plugins/default.3gx` (`sysmodules/rosalina/source/plugin/file_loader.c:14-15`). **Ligar/desligar por jogo = colocar/remover o `.3gx` na pasta daquele título** — exatamente o modelo "o usuário escolhe no front-end".
- `plugin_loader_enabled = 1` já está ativo no Luma do usuário.
- O plugin recebe um bloco de memória de **5 MiB** (`memoryblock.c:12`, ajustável no cabeçalho 3GX): sobra para o agente (~250 KB).
- Rede dentro de um jogo qualquer: o jogo costuma **não** ter acesso ao `soc:U`. O Luma tem a SVC customizada `svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, …, "soc:U")` (`csvc.h`) que permite obter a sessão, e o próprio Rosalina usa rede (`minisoc.c`, stub GDB). **Que um plugin consiga abrir sockets em jogos arbitrários é a grande incógnita — precisa de um teste.**
- Limites conhecidos/prováveis: só vale para o jogo **lançado depois** de ligar o plugin; compatibilidade varia por título (memória, crash); homebrew via HBL usa o título `hbldr_3dsx_titleid` (`000400000d921e00` na config do usuário), então um plugin nessa pasta valeria para **todo** 3DSX.
- Ferramental: `3gxtool`/CTRPluginFramework **não estão** nos repositórios do devkitPro (busca vazia) — teriam que ser obtidos/compilados do GitHub.
- Front-end: `/luma` é `never_write` para a rede. Ligar/desligar exige **destravar só `/luma/plugins/`** com confirmação no console (a "zona de risco" já prevista) e usar `FS_WRITE`/`FS_DELETE`. Listar os jogos instalados exige um comando novo (`AM_GetTitleList`).

## O que NÃO se aplica
Sysmodule residente (ver acima) e, para jogos comerciais, "logs" no sentido de arquivo de log — eles não têm; o que se ganha é acesso ao SD/estado enquanto o jogo roda.
