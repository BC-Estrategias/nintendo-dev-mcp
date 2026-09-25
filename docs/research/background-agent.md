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
